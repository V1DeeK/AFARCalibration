#include "demod.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace {
// app_state.hpp's PI is a float (~7 significant digits) — fine for the
// envelope/threshold math it was written for, but NCO phase accumulators
// that run continuously for the whole file (chroma burst-lock, audio
// subcarrier) need full double precision: the ~1e-7 relative error from
// casting a float constant compounds sample-by-sample and after tens of
// millions of samples adds up to radians of drift, not the negligible
// per-sample error it looks like in isolation.
constexpr double kPiDouble = 3.14159265358979323846;
}

DemodThread::DemodThread(CircularBuffer<CoherentSample>& demod_buf,
                          CircularBuffer<float>& audio_out_buf,
                          AppState& state)
    : demod_buf_(demod_buf), audio_out_buf_(audio_out_buf), state_(state) {}

// public

void DemodThread::run() {
    std::cout << "[demod] started\n";

    while (state_.running.load()) {
        DemodMode mode = state_.demod_mode.load();

        if (mode == DemodMode::Off) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue; // don't burn CPU when demod isn't active
        }

        uint32_t fs        = state_.sample_rate_hz.load();
        bool     deemph_on = state_.fm_deemphasis_on.load();

        bool base_changed = (mode != current_mode_) || (fs != current_sample_rate_) ||
                             (deemph_on != current_deemph_on_);

        if (mode == DemodMode::FmAudio) {
            uint32_t channel_bw = state_.fm_channel_bw_hz.load();
            if (base_changed || channel_bw != current_channel_bw_) {
                reconfigure_fm_audio(fs, channel_bw);
                current_mode_      = mode;
                current_deemph_on_ = deemph_on;
            }
        } else { // TvVideo
            TvStandard standard = state_.tv_standard.load();
            if (base_changed || standard != current_tv_standard_) {
                reconfigure_tv_video(fs, standard);
                current_mode_      = mode;
                current_deemph_on_ = deemph_on;
            }
        }

        if (demod_buf_.available() < kInputChunk) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            continue;
        }

        if (!demod_buf_.pop_batch(batch_.data(), kInputChunk))
            continue;

        select_channel(batch_, channel_iq_);

        decimated_.clear();
        stage1_decimator_.process(channel_iq_.data(), channel_iq_.size(), decimated_);
        if (decimated_.empty())
            continue;

        if (mode == DemodMode::FmAudio) {
            run_fm_audio_path(decimated_);
        } else {
            run_tv_video_path(decimated_);
            if (state_.tv_audio_subcarrier_on.load())
                run_tv_audio_subcarrier_path();
        }
    }

    std::cout << "[demod] exiting\n";
}

// private — shared front end

void DemodThread::select_channel(const std::vector<CoherentSample>& batch, std::vector<Complex>& out) {
    PlutoChannelMode mode = state_.chan_mode.load();
    out.resize(batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i)
        out[i] = (mode == PlutoChannelMode::Channel_2) ? batch[i].rx2 : batch[i].rx1;
}

void DemodThread::configure_audio_tail(double rate) {
    double tau = static_cast<double>(kDeemphasisUs) * 1e-6;
    // Обход де-эмфазиса — это current_deemph_on_ в finish_audio_tail(), а не
    // alpha=1 здесь: kDeemphasisUs константа и вырожденной эта формула не бывает.
    float alpha = static_cast<float>(1.0 - std::exp(-1.0 / (rate * tau)));
    deemphasis_.set_alpha(alpha);
    deemphasis_.reset();

    float cutoff_norm = std::clamp(kAudioBandwidthHz / static_cast<float>(rate / 2.0), 0.01f, 0.9f);
    audio_lpf_.reconfigure(design_lowpass_fir(63, cutoff_norm));

    resampler_.set_rates(rate, kOutputSampleRateHz);

    agc_envelope_      = 0.01f;
    agc_gain_          = 1.0f;
    agc_release_coeff_ = static_cast<float>(1.0 - std::exp(-1.0 / (rate * kAgcReleaseSeconds)));
}

void DemodThread::finish_audio_tail(const std::vector<float>& raw_signal) {
    deemph_out_.clear();
    if (current_deemph_on_) {
        deemph_out_.reserve(raw_signal.size());
        for (float x : raw_signal)
            deemph_out_.push_back(deemphasis_.process(x));
    } else {
        deemph_out_ = raw_signal;
    }

    lpf_out_.clear();
    audio_lpf_.process(deemph_out_.data(), deemph_out_.size(), lpf_out_);
    if (lpf_out_.empty())
        return;

    // automatic gain: recovered amplitude is set by the transmitter's
    // deviation, which for a weak subcarrier can peak under 0.03 — chase a
    // fixed target peak with a peak-hold envelope so quiet sources are
    // actually audible instead of buried near the noise floor. The envelope
    // never falls below the current sample (only leaks down slowly), so the
    // gain computed from it can never let that same sample exceed the
    // target — no clipping by construction. Gain reduction is instant for
    // the same reason (a smoothed reduction would still let a sudden peak
    // clip before gain caught up); gain recovery after the signal quiets
    // down stays slow so it doesn't pump/ride up on noise between passages.
    float envelope_decay = 1.0f - agc_release_coeff_;
    for (float& x : lpf_out_) {
        float ax = std::fabs(x);
        agc_envelope_ = std::max(ax, agc_envelope_ * envelope_decay);

        float target_gain = std::clamp(kAgcTargetPeak / std::max(agc_envelope_, 1e-4f), 0.1f, kAgcMaxGain);
        if (target_gain < agc_gain_)
            agc_gain_ = target_gain;
        else
            agc_gain_ += (target_gain - agc_gain_) * agc_release_coeff_;

        x = std::clamp(x * agc_gain_, -1.0f, 1.0f);
    }

    resampled_.clear();
    resampler_.process(lpf_out_.data(), lpf_out_.size(), resampled_);
    if (resampled_.empty())
        return;

    for (float s : resampled_)
        audio_out_buf_.push(s);

    waveform_ring_.insert(waveform_ring_.end(), resampled_.begin(), resampled_.end());
    if (waveform_ring_.size() > kWaveformWindow)
        waveform_ring_.erase(waveform_ring_.begin(), waveform_ring_.end() - static_cast<long>(kWaveformWindow));

    {
        std::lock_guard<std::mutex> lock(state_.demod_audio_mutex);
        state_.demod_audio_waveform = waveform_ring_;
    }
    state_.demod_audio_has_data.store(true);
}

// private — FM audio path

void DemodThread::reconfigure_fm_audio(uint32_t sample_rate_hz, uint32_t channel_bw_hz) {
    channel_bw_hz  = std::max<uint32_t>(channel_bw_hz, 1000);
    sample_rate_hz = std::max<uint32_t>(sample_rate_hz, channel_bw_hz * 4);

    // stage 1: decimate raw IQ down to ~4x the channel bandwidth, just enough
    // margin for the channel-select filter's transition band
    double target_intermediate = static_cast<double>(channel_bw_hz) * 4.0;
    int    factor               = static_cast<int>(std::floor(static_cast<double>(sample_rate_hz) / target_intermediate));
    stage1_factor_     = std::max(1, factor);
    intermediate_rate_ = static_cast<double>(sample_rate_hz) / static_cast<double>(stage1_factor_);

    // anti-alias / channel-select low-pass ahead of decimation, cutoff at
    // half the channel bandwidth, expressed as a fraction of sample_rate_hz's
    // Nyquist (design_lowpass_fir's cutoff_norm is fraction of Nyquist)
    float cutoff_norm1 = std::clamp(static_cast<float>(channel_bw_hz) / static_cast<float>(sample_rate_hz), 0.001f, 0.45f);
    stage1_decimator_.reconfigure(design_lowpass_fir(63, cutoff_norm1), stage1_factor_);

    // fresh discriminator state — a stale previous-sample phase from before
    // reconfiguration shouldn't glitch the first output sample
    discriminator_ = FmDiscriminator{};

    configure_audio_tail(intermediate_rate_);

    current_sample_rate_ = sample_rate_hz;
    current_channel_bw_  = channel_bw_hz;

    std::cout << "[demod] fm audio reconfigured: fs=" << sample_rate_hz
              << " bw=" << channel_bw_hz
              << " decim=" << stage1_factor_
              << " intermediate_rate=" << intermediate_rate_ << "\n";
}

void DemodThread::run_fm_audio_path(const std::vector<Complex>& decimated_iq) {
    disc_out_.clear();
    discriminator_.process(decimated_iq.data(), decimated_iq.size(), disc_out_);
    finish_audio_tail(disc_out_);
}

// private — TV video path

void DemodThread::reconfigure_tv_video(uint32_t sample_rate_hz, TvStandard standard) {
    // stage 1: light decimation, just enough to keep compute bounded while
    // preserving up to ~7-8 MHz of content (video luma + audio subcarrier).
    // Operationally this needs sample_rate_hz set to at least ~12-16 MHz in
    // the existing Sample Rate control for a usable picture — a precondition
    // of this mode, not something this code enforces.
    constexpr double kTargetIntermediate = 16'000'000.0;
    int factor = static_cast<int>(std::floor(static_cast<double>(sample_rate_hz) / kTargetIntermediate));
    stage1_factor_     = std::clamp(factor, 1, 4);
    intermediate_rate_ = static_cast<double>(sample_rate_hz) / static_cast<double>(stage1_factor_);

    if (stage1_factor_ == 1) {
        // no decimation actually happens at factor 1, so an anti-alias
        // filter has nothing to protect against — skip straight to a
        // 1-tap passthrough instead of spending a full 31-tap convolution
        // per sample (at the raw, undecimated rate) on a near-all-pass
        // filter that exists solely to prevent aliasing during downsampling
        stage1_decimator_.reconfigure({ 1.0f }, 1);
    } else {
        float cutoff_norm1 = std::clamp(1.0f / static_cast<float>(stage1_factor_), 0.05f, 0.95f);
        stage1_decimator_.reconfigure(design_lowpass_fir(31, cutoff_norm1), stage1_factor_);
    }

    discriminator_             = FmDiscriminator{};
    subcarrier_discriminator_  = FmDiscriminator{};
    subcarrier_nco_phase_      = 0.0;

    // NTSC gets a narrower luma cutoff to reduce (not eliminate — 31 taps'
    // transition band is wider than the guard between 3.1MHz and 3.58MHz,
    // so expect mild residual dot-crawl on sharp edges, same as a real set
    // without a comb filter) cross-luma contamination from the color
    // subcarrier; PAL keeps the original cutoff since it never runs the
    // chroma path below
    // Узкий срез нужен ТОЛЬКО когда реально идёт цветовой декод: он стоит
    // трети горизонтальной чёткости, а защищать от цветовой поднесущей нечего,
    // если вспышки в сигнале нет (все эфирные записи в data/ монохромные).
    // Поэтому полоса переключается по факту захвата цвета — см. apply_luma_bandwidth().
    tv_luma_narrow_ = false;
    float luma_cutoff_hz = kVideoBandwidthHz;
    float cutoff_norm_video = std::clamp(luma_cutoff_hz / static_cast<float>(intermediate_rate_ / 2.0), 0.01f, 0.9f);
    (void)kAssumedSyncWidthUs;
    // 31 taps, not 63 — this filter runs at the full undecimated rate (tens
    // of MHz), so its tap count directly sets a real-time compute budget;
    // 31 is already what stage1_decimator_ uses at a similar cutoff and is
    // plenty sharp for the luma-only MVP picture quality. kVideoChromaLpfTaps
    // (not a separate literal) also drives chroma_lpf_ below so both filters'
    // group delays match by construction — see chroma_burst_aligned_'s comment.
    video_lpf_.reconfigure(design_lowpass_fir(kLumaTapsMono, cutoff_norm_video));

    // NTSC color chroma branch
    float cutoff_norm_chroma = std::clamp(kChromaBandwidthHz / static_cast<float>(intermediate_rate_ / 2.0), 0.01f, 0.9f);
    chroma_lpf_.reconfigure(design_lowpass_fir(kVideoChromaLpfTaps, cutoff_norm_chroma), kChromaDecimFactor);

    tv_burst_align_delay_ = static_cast<std::size_t>(kLumaTapsMono - 1) / 2;
    chroma_burst_delay_buf_.assign(std::max<std::size_t>(tv_burst_align_delay_, 1), Complex{ 0.0f, 0.0f });
    chroma_burst_delay_pos_ = 0;

    chroma_nco_phase_ = 0.0;
    tv_burst_accum_   = Complex{ 0.0f, 0.0f };
    tv_chroma_lock_ema_ = 0.0f;
    tv_line_color_valid_ = false;
    state_.tv_color_locked.store(false);

    // audio subcarrier branch: isolate the mixed-down FM subcarrier, then
    // decimate down toward ~500kHz — discriminating directly at
    // intermediate_rate_ (tens of MHz) gives a radians/sample sensitivity so
    // low that realistic subcarrier deviations (tens of kHz) barely register
    constexpr double kSubcarrierTargetRate = 500'000.0;
    subcarrier_decim_factor_ = std::max(1, static_cast<int>(std::floor(intermediate_rate_ / kSubcarrierTargetRate)));
    subcarrier_audio_rate_   = intermediate_rate_ / static_cast<double>(subcarrier_decim_factor_);

    float cutoff_norm_sub = std::clamp(kSubcarrierBandwidthHz / static_cast<float>(intermediate_rate_ / 2.0), 0.01f, 0.9f);
    subcarrier_lpf_.reconfigure(design_lowpass_fir(63, cutoff_norm_sub), subcarrier_decim_factor_);

    configure_audio_tail(subcarrier_audio_rate_);

    // line timing (approximate — MVP, not broadcast-accurate: no interlace,
    // no precise sync/porch widths, just enough to place lines/columns)
    double line_period_us = (standard == TvStandard::PAL) ? 64.0 : 63.5;
    tv_frame_height_ = (standard == TvStandard::PAL) ? 288 : 240;

    tv_nominal_line_samples_ = std::max<std::size_t>(1, static_cast<std::size_t>(line_period_us * 1e-6 * intermediate_rate_));
    tv_porch_samples_        = static_cast<std::size_t>(static_cast<double>(tv_nominal_line_samples_) * 0.08);
    tv_active_video_samples_ = std::max<std::size_t>(1, static_cast<std::size_t>(static_cast<double>(tv_nominal_line_samples_) * 0.82));
    tv_min_sync_samples_     = std::max<std::size_t>(1, static_cast<std::size_t>(4.0e-6 * intermediate_rate_));

    tv_frame_width_ = 320;

    // burst window sits in the porch (breezeway + 9 subcarrier cycles), but
    // tv_line_sample_idx_==0 doesn't correspond to the true porch start —
    // confirmed_sync fires as soon as tv_min_sync_samples_ (4.0us) of solid
    // sync is seen, which for a standard-width (~4.7us) sync pulse is BEFORE
    // the pulse actually ends. That remainder (sync_width - debounce) has to
    // be added to the breezeway or the burst window starts partway inside
    // the trailing edge of the sync pulse instead of the porch, correlating
    // against near-random content instead of the real burst.
    std::size_t assumed_sync_samples = static_cast<std::size_t>(kAssumedSyncWidthUs * 1e-6 * intermediate_rate_);
    std::size_t sync_overshoot_samples = (assumed_sync_samples > tv_min_sync_samples_)
                                              ? (assumed_sync_samples - tv_min_sync_samples_) : 0;
    std::size_t breezeway_samples = static_cast<std::size_t>(0.5e-6 * intermediate_rate_);
    tv_burst_start_samples_  = sync_overshoot_samples + breezeway_samples;
    tv_burst_length_samples_ = static_cast<std::size_t>(9.0 / kChromaSubcarrierHz * intermediate_rate_);

    chroma_enabled_ = (standard == TvStandard::NTSC) &&
                       (tv_burst_start_samples_ + tv_burst_length_samples_ <= tv_porch_samples_);
    if (standard == TvStandard::NTSC && !chroma_enabled_) {
        std::cout << "[demod] NTSC color disabled: burst window (" << (tv_burst_start_samples_ + tv_burst_length_samples_)
                   << " samples) doesn't fit the porch (" << tv_porch_samples_ << " samples) at this sample rate\n";
    }

    tv_working_frame_.assign(static_cast<std::size_t>(tv_frame_width_) * static_cast<std::size_t>(tv_frame_height_) * 3, 0);
    tv_col_accum_.assign(static_cast<std::size_t>(tv_frame_width_), 0.0f);
    tv_col_count_.assign(static_cast<std::size_t>(tv_frame_width_), 0);
    tv_chroma_accum_.assign(static_cast<std::size_t>(tv_frame_width_), Complex{ 0.0f, 0.0f });

    // врезной импульс кадровой синхронизации против строчного: ~27 usec
    // против ~4.7, так что порог в 12 usec разделяет их с большим запасом
    tv_serration_min_samples_ = static_cast<std::size_t>(12.0e-6 * intermediate_rate_);
    // кадровый гасящий интервал по стандарту: 20 строк в NTSC, 24 в PAL
    tv_vbi_lines_ = (standard == TvStandard::PAL) ? 24 : 20;

    tv_env_max_             = 0.1f;
    tv_env_min_             = -0.1f;
    tv_black_level_ema_     = 0.0f;
    tv_sync_level_ema_      = -1.0f;
    tv_sync_accum_          = 0.0f;
    tv_sync_accum_count_    = 0;
    tv_white_env_           = 0.1f;
    tv_line_black_accum_    = 0.0f;
    tv_line_black_count_    = 0;
    tv_sync_low_count_      = 0;
    tv_samples_since_sync_  = 0;
    tv_short_sync_streak_   = 0;
    tv_vsync_fired_         = false;
    tv_row_                 = -1;
    tv_line_active_         = false;
    tv_line_sample_idx_     = 0;
    tv_sample_pos_          = 0;
    tv_field_start_pos_     = 0;
    tv_field_started_       = false;
    tv_last_written_row_    = -1;
    tv_last_line_pos_       = 0;
    tv_have_line_ref_       = false;
    tv_serration_count_     = 0;
    tv_serration_first_pos_ = 0;
    tv_serration_last_pos_  = 0;

    current_sample_rate_ = sample_rate_hz;
    current_tv_standard_ = standard;

    std::cout << "[demod] tv video reconfigured: fs=" << sample_rate_hz
              << " decim=" << stage1_factor_
              << " intermediate_rate=" << intermediate_rate_
              << " line_samples=" << tv_nominal_line_samples_
              << " frame=" << tv_frame_width_ << "x" << tv_frame_height_
              << " subcarrier_decim=" << subcarrier_decim_factor_
              << " subcarrier_audio_rate=" << subcarrier_audio_rate_
              << " color=" << (chroma_enabled_ ? "on" : "off") << "\n";
}

void DemodThread::apply_luma_bandwidth(bool narrow) {
    if (narrow == tv_luma_narrow_)
        return;
    // Меняется только частота среза, число отводов остаётся тем же — групповая
    // задержка не едет, и выравнивание цветовой вспышки (tv_burst_align_delay_)
    // остаётся верным без пересчёта.
    float cutoff_hz   = narrow ? kChromaLumaCutoffHz : kVideoBandwidthHz;
    int   taps        = narrow ? kLumaTapsColor : kLumaTapsMono;
    float cutoff_norm = std::clamp(cutoff_hz / static_cast<float>(intermediate_rate_ / 2.0), 0.01f, 0.9f);
    video_lpf_.reconfigure(design_lowpass_fir(taps, cutoff_norm));

    // Выравнивание цветовой вспышки идёт по задержке ИМЕННО фильтра яркости —
    // на его выходе живёт tv_line_sample_idx_, по которому отсчитывается окно
    // вспышки. В цветном режиме отводов поровну, так что задержки совпадают и
    // цветовые столбцы тоже выровнены; в монохромном цветовая ветвь отстаёт,
    // но её результат всё равно не отображается.
    tv_burst_align_delay_ = static_cast<std::size_t>(taps - 1) / 2;
    chroma_burst_delay_buf_.assign(std::max<std::size_t>(tv_burst_align_delay_, 1), Complex{ 0.0f, 0.0f });
    chroma_burst_delay_pos_ = 0;
    tv_luma_narrow_ = narrow;
    std::cout << "[demod] luma bandwidth -> " << (narrow ? "3.1 MHz (color decode active)"
                                                          : "5 MHz (monochrome, sharper)") << "\n";
}

void DemodThread::run_tv_video_path(const std::vector<Complex>& decimated_iq) {
    // сужаем полосу яркости только пока цвет действительно захвачен
    apply_luma_bandwidth(chroma_enabled_ && tv_line_color_valid_);

    disc_out_.clear();
    discriminator_.process(decimated_iq.data(), decimated_iq.size(), disc_out_);

    video_lpf_out_.clear();
    video_lpf_.process(disc_out_.data(), disc_out_.size(), video_lpf_out_);

    if (chroma_enabled_) {
        // NCO-mix disc_out_ to chroma baseband — same rotating-complex-multiply
        // idiom as run_tv_audio_subcarrier_path's osc/rot pattern below (cos/sin
        // per sample was a measured real-time bottleneck already at this rate)
        double w = 2.0 * kPiDouble * kChromaSubcarrierHz / intermediate_rate_;
        Complex osc(static_cast<float>(std::cos(chroma_nco_phase_)),
                    static_cast<float>(-std::sin(chroma_nco_phase_)));
        const Complex rot(static_cast<float>(std::cos(w)), static_cast<float>(-std::sin(w)));

        chroma_mixed_.clear();
        chroma_mixed_.reserve(disc_out_.size());
        for (float x : disc_out_) {
            chroma_mixed_.push_back(Complex(x, 0.0f) * osc);
            osc *= rot;
        }
        chroma_nco_phase_ += w * static_cast<double>(disc_out_.size());
        chroma_nco_phase_ = std::fmod(chroma_nco_phase_, 2.0 * PI);

        chroma_lpf_out_.clear();
        chroma_lpf_.process(chroma_mixed_.data(), chroma_mixed_.size(), chroma_lpf_out_);

        // hold-expand the decimated chroma baseband back to one value per
        // luma sample — process_video_sample's column binning indexes it
        // 1:1 against video_lpf_out_[i], and color content varies far too
        // slowly (kChromaBandwidthHz) to need per-sample resolution here
        // anyway (see kChromaDecimFactor's declaration). A batch boundary
        // can land mid-hold and leave fewer decimated outputs than a full
        // kChromaDecimFactor-way split would need — pad the tail with the
        // last known value (or zero, on the very first batch before any
        // decimated output exists yet) rather than leaving it short.
        chroma_lpf_out_full_.clear();
        chroma_lpf_out_full_.reserve(chroma_mixed_.size());
        for (const Complex& c : chroma_lpf_out_) {
            std::size_t remaining = chroma_mixed_.size() - chroma_lpf_out_full_.size();
            std::size_t reps      = std::min<std::size_t>(kChromaDecimFactor, remaining);
            chroma_lpf_out_full_.insert(chroma_lpf_out_full_.end(), reps, c);
        }
        Complex hold_value = chroma_lpf_out_full_.empty() ? Complex{ 0.0f, 0.0f } : chroma_lpf_out_full_.back();
        chroma_lpf_out_full_.resize(chroma_mixed_.size(), hold_value);

        // chroma_mixed_ is undelayed relative to disc_out_, but tv_line_sample_idx_
        // runs on video_lpf_out_'s clock (delayed by (taps-1)/2 samples) — apply
        // the same delay to a second copy so burst accumulation reads the right
        // physical porch samples instead of (taps-1)/2 samples too late
        chroma_burst_aligned_.clear();
        chroma_burst_aligned_.reserve(chroma_mixed_.size());
        if (tv_burst_align_delay_ == 0) {
            chroma_burst_aligned_ = chroma_mixed_;
        } else {
            std::size_t ring_len = chroma_burst_delay_buf_.size();
            for (const Complex& x : chroma_mixed_) {
                chroma_burst_aligned_.push_back(chroma_burst_delay_buf_[chroma_burst_delay_pos_]);
                chroma_burst_delay_buf_[chroma_burst_delay_pos_] = x;
                chroma_burst_delay_pos_ = (chroma_burst_delay_pos_ + 1 == ring_len) ? 0 : chroma_burst_delay_pos_ + 1;
            }
        }
    }

    for (std::size_t i = 0; i < video_lpf_out_.size(); ++i) {
        Complex chroma_baseband  = chroma_enabled_ ? chroma_lpf_out_full_[i] : Complex{ 0.0f, 0.0f };
        Complex chroma_burst_raw = chroma_enabled_ ? chroma_burst_aligned_[i] : Complex{ 0.0f, 0.0f };
        process_video_sample(video_lpf_out_[i], chroma_baseband, chroma_burst_raw);
    }
}

void DemodThread::process_video_sample(float x, Complex chroma_baseband, Complex chroma_burst_raw) {
    // adaptive envelope tracker: both bounds leak toward the current sample
    // each tick unless refreshed by a more extreme one, giving a running
    // amplitude range that tolerates slow gain drift without a fixed
    // calibration step
    constexpr float kLeak = 0.0005f;
    tv_env_max_ = std::max(x, tv_env_max_ - kLeak);
    tv_env_min_ = std::min(x, tv_env_min_ + kLeak);
    float range = std::max(tv_env_max_ - tv_env_min_, 1e-6f);
    float norm  = ((x - tv_env_min_) / range) * 2.0f - 1.0f; // -1..+1

    ++tv_sample_pos_;

    bool below = norm < state_.tv_sync_threshold.load();
    if (below) {
        // накапливаем сам синхроимпульс — он и есть опора чёрного для строки.
        // Аккумулятор обнуляется в начале КАЖДОГО провала: ниже порога уходят
        // и тёмные детали изображения, и если тащить их в ту же сумму, опора
        // уезжает вверх, размах "гашение-синхро" занижается и картинка
        // получается пересвеченной.
        if (tv_sync_low_count_ == 0) {
            tv_sync_accum_       = 0.0f;
            tv_sync_accum_count_ = 0;
        }
        ++tv_sync_low_count_;
        // только середина импульса: спад занимает больше микросекунды из
        // четырёх с небольшим, и если считать его вместе с вершиной, опора
        // уезжает вверх (замерено: -0.32 вместо истинных -0.47), размах
        // "гашение-синхро" занижается и вся яркостная шкала едет
        if (tv_sync_low_count_ * 2 >= tv_min_sync_samples_) {
            tv_sync_accum_ += x;
            ++tv_sync_accum_count_;
        }
    }
    bool confirmed_sync = (tv_sync_low_count_ == tv_min_sync_samples_);

    // конец провала: по его длительности отличаем строчный импульс от
    // врезного импульса кадровой синхронизации (~27 usec против ~4.7)
    if (!below && tv_sync_low_count_ > 0) {
        if (tv_sync_low_count_ >= tv_serration_min_samples_) {
            uint64_t pos = tv_sample_pos_ - tv_sync_low_count_;
            // импульсы дальше 20 строк друг от друга — уже разные группы
            if (tv_serration_count_ > 0 &&
                pos - tv_serration_last_pos_ > 20 * tv_nominal_line_samples_) {
                tv_serration_count_ = 0;
            }
            if (tv_serration_count_ == 0)
                tv_serration_first_pos_ = pos;
            tv_serration_last_pos_ = pos;
            ++tv_serration_count_;

            if (!tv_vsync_fired_ && tv_serration_count_ >= kVsyncStreakThreshold)
                start_new_field(tv_serration_first_pos_);
        }
        tv_sync_low_count_ = 0;
    }

    if (confirmed_sync) {
        std::size_t interval  = tv_samples_since_sync_;
        tv_samples_since_sync_ = 0;

        // синтетика mp4_to_iq.py кадровую синхронизацию врезными импульсами не
        // кодирует — она шлёт пачку коротких импульсов подряд, поэтому старый
        // признак остаётся рабочим вторым путём
        bool short_interval = interval < (tv_nominal_line_samples_ / 2);
        tv_short_sync_streak_ = short_interval ? tv_short_sync_streak_ + 1 : 0;
        if (short_interval) {
            if (!tv_vsync_fired_ && tv_short_sync_streak_ >= kVsyncStreakThreshold)
                start_new_field(tv_sample_pos_);
            return;
        }
        tv_vsync_fired_ = false;

        // уровень вершины синхроимпульса этой строки
        if (tv_sync_accum_count_ > 0) {
            float lvl = tv_sync_accum_ / static_cast<float>(tv_sync_accum_count_);
            tv_sync_level_ema_ += (lvl - tv_sync_level_ema_) * kSyncLevelEmaAlpha;
        }

        // маховик: строка принимается, только если пришла в ожидаемое окно
        uint64_t pos = tv_sample_pos_;
        int      row = -1;
        if (tv_have_line_ref_) {
            double  spacing = static_cast<double>(pos - tv_last_line_pos_) / static_cast<double>(tv_nominal_line_samples_);
            double  k       = std::round(spacing);
            if (k < 1.0 || std::fabs(spacing - k) > kLinePhaseTolerance) {
                ++tv_diag_rejected_;
                return;             // не по сетке — ложное срабатывание
            }
            // период строки меряем по самому сигналу: расхождение с
            // номинальными 63.5/64 usec — первый признак того, что частота
            // дискретизации не та, на которой сделана запись
            float measured = static_cast<float>((pos - tv_last_line_pos_) / k);
            tv_diag_line_samples_ = (tv_diag_line_samples_ <= 0.0f)
                                        ? measured
                                        : tv_diag_line_samples_ + (measured - tv_diag_line_samples_) * kDiagEmaAlpha;
        }
        tv_last_line_pos_ = pos;
        tv_have_line_ref_ = true;

        if (tv_field_started_) {
            double rel = static_cast<double>(pos - tv_field_start_pos_) / static_cast<double>(tv_nominal_line_samples_);
            row = static_cast<int>(std::lround(rel)) - static_cast<int>(tv_vbi_lines_);
        }

        tv_row_ = row;
        tv_col_accum_.assign(tv_col_accum_.size(), 0.0f);
        tv_col_count_.assign(tv_col_count_.size(), 0);
        tv_chroma_accum_.assign(tv_chroma_accum_.size(), Complex{ 0.0f, 0.0f });
        tv_burst_accum_       = Complex{ 0.0f, 0.0f };
        tv_line_black_accum_  = 0.0f;
        tv_line_black_count_  = 0;
        tv_line_active_       = true;
        tv_line_sample_idx_   = 0;
        return; // the sync-tip sample itself isn't video content
    }

    ++tv_samples_since_sync_;

    if (!tv_line_active_ || tv_row_ < 0 || tv_row_ >= tv_frame_height_)
        return;

    ++tv_line_sample_idx_;

    // measure this line's true black (blanking) level from the flat back-
    // porch region AFTER the color burst and before active video begins —
    // see tv_black_level_ema_'s declaration for why this, not tv_env_min_
    // (the sync tip), is the real black reference. Sampling right after the
    // sync pulse instead (before the burst) sits too close to that edge:
    // the luma LPF is still ringing from the sync transition there, which
    // measured as a bias that never settled and kept dragging the EMA more
    // negative line after line (verified with the color_diag harness) —
    // this window sits ~50 samples further from the edge, past the burst,
    // which is comfortably enough for an 11-tap filter to have settled.
    // accumulated in raw discriminator units (x), not norm — norm's scale
    // depends on tv_env_max_, which chases whatever sample last exceeded it
    // and decays back down (kLeak) within about a line's worth of samples,
    // so it never settles on true peak white unless bright content refreshes
    // it constantly. Measured directly (verified with the color_diag
    // harness: batches of raw disc_out_/video_lpf_out_ reach up to ~0.3-0.5
    // on real bright content, but the persistent leaky tv_env_max_ never
    // reflects that, sitting pinned near 0 instead) — so brightness here is
    // computed from raw x against two stable references instead: this
    // measured black point, and a white point derived analytically below.
    std::size_t line_burst_end = tv_burst_start_samples_ + tv_burst_length_samples_;
    if (tv_line_sample_idx_ > line_burst_end && tv_line_sample_idx_ <= tv_porch_samples_) {
        tv_line_black_accum_ += x;
        ++tv_line_black_count_;
        if (tv_line_sample_idx_ == tv_porch_samples_ && tv_line_black_count_ > 0) {
            float line_black = tv_line_black_accum_ / static_cast<float>(tv_line_black_count_);
            tv_black_level_ema_ += (line_black - tv_black_level_ema_) * kBlackLevelEmaAlpha;
        }
    }

    if (chroma_enabled_) {
        std::size_t burst_end = tv_burst_start_samples_ + tv_burst_length_samples_;
        if (tv_line_sample_idx_ > tv_burst_start_samples_ && tv_line_sample_idx_ <= burst_end) {
            tv_burst_accum_ += chroma_burst_raw;
            if (tv_line_sample_idx_ == burst_end) {
                // Delta-phi between the encoder's burst reference and our
                // free-running NCO — measured fresh every line (not
                // smoothed across lines: real phase drift moves on a
                // timescale of many lines, so a per-line snap reacts
                // immediately to a bad/noisy single burst via the
                // confidence gate below instead of polluting neighbors
                // with a stale averaged value)
                tv_line_phase_correction_ = std::atan2(tv_burst_accum_.imag(), tv_burst_accum_.real());
                float confidence = std::abs(tv_burst_accum_) / static_cast<float>(tv_burst_length_samples_);
                tv_chroma_lock_ema_ += (confidence - tv_chroma_lock_ema_) * kChromaLockEmaAlpha;
                tv_line_color_valid_ = tv_chroma_lock_ema_ > kChromaLockThreshold;
                state_.tv_color_locked.store(tv_line_color_valid_);
            }
        }
    }

    if (tv_line_sample_idx_ < tv_porch_samples_)
        return;

    std::size_t pos = tv_line_sample_idx_ - tv_porch_samples_;
    if (pos < tv_active_video_samples_) {
        int col = static_cast<int>(pos * static_cast<std::size_t>(tv_frame_width_) / tv_active_video_samples_);
        col = std::clamp(col, 0, tv_frame_width_ - 1);
        tv_col_accum_[static_cast<std::size_t>(col)] += x;
        tv_col_count_[static_cast<std::size_t>(col)] += 1;
        if (chroma_enabled_)
            tv_chroma_accum_[static_cast<std::size_t>(col)] += chroma_baseband;
        return;
    }

    // active video window closed — flush the accumulated columns into this row

    // Уровень белого. Опора — размах "уровень гашения минус вершина
    // синхроимпульса", измеренный по самому сигналу: в настоящем композитном
    // сигнале белое стоит на 100 IRE при синхроимпульсе 40 IRE, то есть в 2.5
    // раза выше этого размаха, а файлы mp4_to_iq.py кодируют белое как 0.85
    // того же размаха — отсюда настройка, а не константа. Прежний код брал
    // 0.85 всегда и отсчитывал размах от tv_env_min_ (leaky-трекер минимума),
    // из-за чего на реальных записях всё активное видео оказывалось выше
    // расчётного белого и картинка была сплошь белой.
    float unit  = tv_black_level_ema_ - tv_sync_level_ema_;
    float white;
    if (state_.tv_white_auto.load()) {
        float peak = 0.0f;
        for (int c = 0; c < tv_frame_width_; ++c) {
            std::size_t uc = static_cast<std::size_t>(c);
            if (tv_col_count_[uc] > 0)
                peak = std::max(peak, tv_col_accum_[uc] / static_cast<float>(tv_col_count_[uc]));
        }
        tv_white_env_ = std::max(peak, tv_white_env_ * kWhiteAutoDecay);
        white = tv_white_env_;
    } else {
        white = tv_black_level_ema_ + state_.tv_white_ratio.load() * unit;
    }
    float headroom = std::max(white - tv_black_level_ema_, 1e-6f);

    std::size_t row_offset = static_cast<std::size_t>(tv_row_) * static_cast<std::size_t>(tv_frame_width_) * 3;
    // undoes the encoder's kChromaGain scaling and the demod's own 1/2
    // amplitude loss from the quadrature mix+LPF (see the plan's derivation:
    // chroma_lpf_out ~= (1/2)*S*e^{jDeltaPhi}, S = U - jV)
    Complex correction = std::polar(2.0f / kChromaGain, -tv_line_phase_correction_);
    // столбец без единого отсчёта повторяет предыдущий: при низкой частоте
    // дискретизации отсчётов в строке может быть меньше, чем столбцов, и
    // пустые ушли бы в кадр вертикальными чёрными полосами
    float prev_avg = tv_black_level_ema_;
    for (int c = 0; c < tv_frame_width_; ++c) {
        std::size_t uc  = static_cast<std::size_t>(c);
        float       avg = tv_col_count_[uc] > 0 ? tv_col_accum_[uc] / static_cast<float>(tv_col_count_[uc]) : prev_avg;
        prev_avg = avg;
        float       gray01 = std::clamp((avg - tv_black_level_ema_) / headroom, 0.0f, 1.0f);
        float       y = gray01 * 255.0f;

        uint8_t r, g, b;
        if (tv_line_color_valid_) {
            Complex s_est = (tv_chroma_accum_[uc] / static_cast<float>(std::max(tv_col_count_[uc], 1))) * correction;
            // s_est recovers U/V in the encoder's [0,1]-normalized scale
            // (computed from 0..1 R/G/B there) — y here is already 0..255,
            // so U/V need the same *255 to not be swamped to invisibility
            // against y in the matrix below
            float u = s_est.real() * 255.0f;
            float v = -s_est.imag() * 255.0f;
            r = static_cast<uint8_t>(std::clamp(y + 1.140f * v, 0.0f, 255.0f));
            g = static_cast<uint8_t>(std::clamp(y - 0.395f * u - 0.581f * v, 0.0f, 255.0f));
            b = static_cast<uint8_t>(std::clamp(y + 2.032f * u, 0.0f, 255.0f));
        } else {
            r = g = b = static_cast<uint8_t>(y);
        }
        tv_working_frame_[row_offset + uc * 3 + 0] = r;
        tv_working_frame_[row_offset + uc * 3 + 1] = g;
        tv_working_frame_[row_offset + uc * 3 + 2] = b;
    }

    // Непойманные строки дублируем этой: одиночная чёрная строка посреди
    // картинки мигает и заметна сильнее, чем повтор соседней. Но только
    // мелкие пропуски — на разрыве записи теряются десятки строк подряд, и
    // растягивание одной строки на полкадра выглядит хуже честного чёрного
    // поля и прячет сам факт потери.
    constexpr int kMaxDuplicatedRows = 4;
    int fill_from = std::max(tv_last_written_row_ + 1, tv_row_ - kMaxDuplicatedRows);
    for (int r = fill_from; r < tv_row_; ++r) {
        std::size_t dst = static_cast<std::size_t>(r) * static_cast<std::size_t>(tv_frame_width_) * 3;
        std::copy(tv_working_frame_.begin() + static_cast<long>(row_offset),
                  tv_working_frame_.begin() + static_cast<long>(row_offset) +
                      static_cast<long>(tv_frame_width_) * 3,
                  tv_working_frame_.begin() + static_cast<long>(dst));
    }
    tv_last_written_row_ = tv_row_;
    tv_line_active_ = false; // wait for the next confirmed hsync before capturing another line
}

void DemodThread::start_new_field(uint64_t field_start_pos) {
    // Кадровая синхронизация не может прийти раньше, чем через поле (262.5
    // строки в NTSC, 312.5 в PAL). Всё, что раньше, — ложное срабатывание на
    // тёмном участке картинки: достаточно длинный провал яркости неотличим от
    // врезного импульса поодиночке. Без этой проверки поле начиналось дважды,
    // и измеренная частота полей выходила вдвое выше настоящей (117 Hz вместо
    // 59.94), а кадр терял нижнюю часть.
    double nominal_field = ((current_tv_standard_ == TvStandard::PAL) ? 312.5 : 262.5)
                            * static_cast<double>(tv_nominal_line_samples_);
    if (tv_field_started_ && field_start_pos > tv_field_start_pos_ &&
        static_cast<double>(field_start_pos - tv_field_start_pos_) < 0.5 * nominal_field) {
        tv_serration_count_ = 0;
        return;
    }

    // огрызок в несколько строк — не кадр: на рваном сигнале две кадровые
    // синхронизации могут прийти почти подряд, и такой "кадр" даёт только
    // чёрный экран поверх нормальной картинки
    if (tv_last_written_row_ >= tv_frame_height_ / 4)
        publish_tv_frame();

    // --- диагностика за только что закрывшееся поле ---
    if (tv_diag_prev_field_pos_ != 0 && field_start_pos > tv_diag_prev_field_pos_) {
        float period = static_cast<float>(field_start_pos - tv_diag_prev_field_pos_);
        tv_diag_field_samples_ = (tv_diag_field_samples_ <= 0.0f)
                                     ? period
                                     : tv_diag_field_samples_ + (period - tv_diag_field_samples_) * kDiagEmaAlpha;
        // Поле, заметно более короткое, чем обычно даёт этот источник, значит
        // из потока выпал кусок (диск не успел за записью или переполнился
        // буфер). Сравниваем именно со средним по источнику, а не со
        // стандартными 262.5 строки: синтетика из mp4_to_iq.py укладывает в
        // поле около 248 строк и по стандартному критерию вечно числилась бы
        // рваной. Первые поля после запуска идут с неустановившейся
        // синхронизацией, по ним судить нельзя.
        if (++tv_diag_field_count_ > 4)
            state_.tv_diag_stream_gap.store(period < 0.90f * tv_diag_field_samples_);
    }
    tv_diag_prev_field_pos_ = field_start_pos;

    state_.tv_diag_line_us.store(tv_diag_line_samples_ > 0.0f
                                     ? static_cast<float>(tv_diag_line_samples_ / intermediate_rate_ * 1e6) : 0.0f);
    state_.tv_diag_field_hz.store(tv_diag_field_samples_ > 0.0f
                                      ? static_cast<float>(intermediate_rate_ / tv_diag_field_samples_) : 0.0f);
    state_.tv_diag_sync_level.store(tv_sync_level_ema_);
    state_.tv_diag_black_level.store(tv_black_level_ema_);
    state_.tv_diag_lines_captured.store(tv_last_written_row_ + 1);
    state_.tv_diag_lines_rejected.store(tv_diag_rejected_);
    tv_diag_rejected_ = 0;

    // строки нового поля нумеруются от начала кадровой синхронизации, так что
    // достаточно запомнить её позицию — дальше номер строки считается по
    // времени, а не накоплением
    tv_field_start_pos_  = field_start_pos;
    tv_field_started_    = true;
    tv_vsync_fired_      = true;
    tv_row_              = -1;
    tv_last_written_row_ = -1;
    tv_line_active_      = false;
    tv_serration_count_  = 0;
    std::fill(tv_working_frame_.begin(), tv_working_frame_.end(), 0);
}

void DemodThread::repair_bad_lines(std::vector<uint8_t>& frame) const {
    // Импульсная помеха портит строку целиком (в FM это щелчок на всю
    // длительность), и такая строка резко выбивается из соседей. Критерий
    // сравнительный, а не абсолютный: строка считается битой, только если она
    // далека от ОБОИХ соседей, а те при этом похожи друг на друга — иначе под
    // раздачу попадала бы любая горизонтальная граница в кадре.
    const std::size_t stride = static_cast<std::size_t>(tv_frame_width_) * 3;
    if (tv_frame_height_ < 3)
        return;

    for (int r = 1; r < tv_frame_height_ - 1; ++r) {
        const uint8_t* up   = frame.data() + (static_cast<std::size_t>(r) - 1) * stride;
        uint8_t*       cur  = frame.data() + static_cast<std::size_t>(r) * stride;
        const uint8_t* down = frame.data() + (static_cast<std::size_t>(r) + 1) * stride;

        long diff_neighbors = 0, diff_cur = 0;
        for (std::size_t i = 0; i < stride; i += 3) { // хватит одного канала
            diff_neighbors += std::abs(static_cast<int>(up[i]) - static_cast<int>(down[i]));
            diff_cur       += std::abs(static_cast<int>(cur[i]) - (static_cast<int>(up[i]) + static_cast<int>(down[i])) / 2);
        }
        float n = static_cast<float>(tv_frame_width_);
        float dn = static_cast<float>(diff_neighbors) / n;
        float dc = static_cast<float>(diff_cur) / n;

        if (dc > 3.0f * dn + 12.0f) {
            for (std::size_t i = 0; i < stride; ++i)
                cur[i] = static_cast<uint8_t>((static_cast<int>(up[i]) + static_cast<int>(down[i])) / 2);
        }
    }
}

void DemodThread::apply_temporal_nr(std::vector<uint8_t>& frame, float strength) {
    if (tv_prev_frame_.size() != frame.size()) {
        tv_prev_frame_ = frame;
        return;
    }
    // Усреднение с предыдущим кадром гасит шум, но по движущимся местам его
    // включать нельзя — иначе за движением тянется шлейф. Поэтому фильтр
    // отключается там, где кадры разошлись сильнее порога: шум держится в
    // единицах уровней, реальное движение даёт заметно больше.
    constexpr int kMotionThreshold = 24;
    float a = std::clamp(strength, 0.0f, 0.95f);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        int cur = frame[i], prev = tv_prev_frame_[i];
        if (std::abs(cur - prev) < kMotionThreshold)
            frame[i] = static_cast<uint8_t>(prev + static_cast<float>(cur - prev) * (1.0f - a));
    }
    tv_prev_frame_ = frame;
}

void DemodThread::publish_tv_frame() {
    tv_publish_frame_ = tv_working_frame_;

    if (state_.tv_repair_bad_lines.load())
        repair_bad_lines(tv_publish_frame_);
    float nr = state_.tv_temporal_nr.load();
    if (nr > 0.01f)
        apply_temporal_nr(tv_publish_frame_, nr);
    else if (!tv_prev_frame_.empty())
        tv_prev_frame_.clear(); // фильтр выключили — не тащить в него старый кадр

    {
        std::lock_guard<std::mutex> lock(state_.tv_frame_mutex);
        state_.tv_frame        = tv_publish_frame_;
        state_.tv_frame_width  = tv_frame_width_;
        state_.tv_frame_height = tv_frame_height_;
    }
    state_.tv_frame_has_data.store(true);
    state_.tv_frame_update_count.fetch_add(1);
}

void DemodThread::run_tv_audio_subcarrier_path() {
    double offset = state_.tv_audio_subcarrier_offset_hz.load();
    double w      = 2.0 * kPiDouble * offset / intermediate_rate_;

    // rotate a unit complex number by a fixed per-sample increment instead of
    // calling cos/sin per sample — this loop runs at the full undecimated
    // rate (tens of MHz), where two transcendental calls per sample was a
    // significant fraction of the real-time budget. Re-deriving the exact
    // osc value from subcarrier_nco_phase_ once per call (rather than
    // trusting the rotation across many calls) keeps the usual float
    // rotation-drift from ever accumulating beyond a few thousand samples.
    Complex osc(static_cast<float>(std::cos(subcarrier_nco_phase_)),
                static_cast<float>(-std::sin(subcarrier_nco_phase_)));
    const Complex rot(static_cast<float>(std::cos(w)), static_cast<float>(-std::sin(w)));

    subcarrier_mixed_.clear();
    subcarrier_mixed_.reserve(disc_out_.size());
    for (float x : disc_out_) {
        subcarrier_mixed_.push_back(Complex(x, 0.0f) * osc);
        osc *= rot;
    }

    subcarrier_nco_phase_ += w * static_cast<double>(disc_out_.size());
    subcarrier_nco_phase_ = std::fmod(subcarrier_nco_phase_, 2.0 * PI);

    subcarrier_lpf_out_.clear();
    subcarrier_lpf_.process(subcarrier_mixed_.data(), subcarrier_mixed_.size(), subcarrier_lpf_out_);
    if (subcarrier_lpf_out_.empty())
        return;

    subcarrier_disc_out_.clear();
    subcarrier_discriminator_.process(subcarrier_lpf_out_.data(), subcarrier_lpf_out_.size(), subcarrier_disc_out_);

    finish_audio_tail(subcarrier_disc_out_);
}
