#pragma once

#include "app_state.hpp"
#include "circular_buffer.hpp"
#include "dsp_utils.hpp"
#include "sample.hpp"

#include <cstdint>
#include <vector>

// Demodulation thread — FM audio and TV video (monochrome MVP). Shape
// mirrors ProcessingThread/DirectionFindingThread: poll config atomics ->
// rebuild derived DSP state on change -> available()-gated pop_batch -> DSP
// -> publish under a dedicated AppState mutex. Both modes share an
// identical front end (decimate -> FM-discriminate) and only diverge after
// that, and only one mode is ever active at a time, so one thread with a
// runtime mode switch handles both rather than two separate threads.
class DemodThread {
public:
    DemodThread(CircularBuffer<CoherentSample>& demod_buf,
                CircularBuffer<float>& audio_out_buf,
                AppState& state);

    void run();

private:
    // shared front end
    void select_channel(const std::vector<CoherentSample>& batch, std::vector<Complex>& out);
    void configure_audio_tail(double rate); // de-emphasis/LPF/resampler, shared by both modes

    // FM audio path
    void reconfigure_fm_audio(uint32_t sample_rate_hz, uint32_t channel_bw_hz);
    void run_fm_audio_path(const std::vector<Complex>& decimated_iq);
    void finish_audio_tail(const std::vector<float>& raw_signal); // de-emphasis -> LPF -> resample -> publish/push

    // TV video path
    void reconfigure_tv_video(uint32_t sample_rate_hz, TvStandard standard);
    void run_tv_video_path(const std::vector<Complex>& decimated_iq);
    void apply_luma_bandwidth(bool narrow); // узкая полоса только при активном цветовом декоде
    // sync detection + line/frame assembly state machine; chroma_baseband/
    // chroma_burst_raw are zeroed Complex{} when chroma_enabled_ is false
    // (PAL, or NTSC where the burst window doesn't fit the porch) so the
    // rest of the state machine doesn't need to branch on standard at all
    void process_video_sample(float luma_x, Complex chroma_baseband, Complex chroma_burst_raw);
    void start_new_field(uint64_t field_start_pos); // публикует кадр и начинает следующее поле
    void publish_tv_frame();
    void run_tv_audio_subcarrier_path(); // reuses disc_out_ populated by run_tv_video_path this iteration

    CircularBuffer<CoherentSample>& demod_buf_;
    CircularBuffer<float>&          audio_out_buf_;
    AppState&                       state_;

    // input chunk popped from demod_buf_ each iteration (independent of fft_size)
    static constexpr std::size_t kInputChunk = 4096;
    std::vector<CoherentSample>  batch_{ std::vector<CoherentSample>(kInputChunk) };

    static constexpr float  kAudioBandwidthHz      = 15000.0f;
    // Постоянная времени де-эмфазиса. 75 мкс — стандарт вещания в NTSC/ATSC-регионах;
    // на 50 мкс (МСЭ-R BS.450, Европа) меняется здесь, а не из UI: переключателя нет.
    static constexpr int    kDeemphasisUs          = 75;
    static constexpr float  kVideoBandwidthHz      = 5'000'000.0f;  // PAL luma cutoff (PAL stays monochrome — no chroma path)
    static constexpr float  kSubcarrierBandwidthHz = 150'000.0f;

    // NTSC color decode — burst-locked (works on real drifting RF captures,
    // not just this project's own synthetic mp4_to_iq.py files, since the
    // phase reference is measured from each line's color burst rather than
    // assumed to match a free-running local oscillator)
    static constexpr double kChromaSubcarrierHz  = 3'579'545.0;  // must exactly match mp4_to_iq.py's CHROMA_SUBCARRIER_HZ
    static constexpr float  kChromaLumaCutoffHz  = 3'100'000.0;  // NTSC-only luma cutoff (decoder-internal, no encoder counterpart)
    static constexpr float  kChromaBandwidthHz   = 1'200'000.0;  // chroma LPF cutoff (decoder-internal, no encoder counterpart)
    // SAME constant drives video_lpf_ AND chroma_lpf_ so their group delays
    // match by construction (see burst alignment below). 15, not 31 — a
    // real+complex pair of full-rate FIRs at 31 taps measured below 20MHz
    // real-time (~14-16 Msps with the audio subcarrier also on); chroma's
    // 1.2MHz cutoff sits far enough from Nyquist at typical 16-20MHz
    // intermediate rates that a shorter filter still has plenty of
    // transition-band room (matches the precedent already set by
    // stage1_decimator_'s 31 taps at a similarly modest cutoff ratio,
    // just halved further since NOW two of these run per sample, not one)
    static constexpr int    kVideoChromaLpfTaps  = 11;
    // Отводы фильтра яркости зависят от режима (см. apply_luma_bandwidth):
    // в цветном берётся столько же, сколько у цветовой ветви, чтобы групповые
    // задержки совпадали по построению и выравнивание вспышки не требовало
    // пересчёта. В монохромном фильтр длиннее и шире: у 11-отводного АЧХ
    // настолько пологая, что реальный спад начинается около 2 MHz вместо
    // заявленных 3.1, и картинка выходит мыльной по горизонтали (замерено:
    // средний горизонтальный градиент 10.7 против 12.8 у эталонного декодера
    // из python/iq_tv_decode.py на том же кадре).
    static constexpr int    kLumaTapsColor       = kVideoChromaLpfTaps;
    static constexpr int    kLumaTapsMono        = 31;
    // chroma_lpf_ decimation: kChromaBandwidthHz (1.2MHz) is well under
    // Nyquist even after cutting the rate by 4x at typical 16-20MHz
    // intermediate rates (Nyquist would allow up to ~8x), so running the
    // (expensive — complex, full n-tap dot product per sample) chroma FIR at
    // 1/4 rate and holding each output for the 4 skipped samples costs
    // ~4x less than the old factor-1 version, for baseband color content
    // that doesn't need per-sample resolution anyway (unlike burst-lock
    // phase, which reads straight off the undecimated NCO-mixed signal via
    // chroma_burst_aligned_ and is untouched by this).
    static constexpr int    kChromaDecimFactor    = 4;
    static constexpr float  kChromaGain          = 0.5f;         // must exactly match mp4_to_iq.py's CHROMA_GAIN
    // EMA burst-correlation confidence -> tv_color_locked_. Theoretical max
    // for a burst_amplitude-0.3 signal is ~0.15 (amplitude/2), but real
    // filter rolloff/windowing loses some of that — measured ~0.135-0.14 on
    // a clean synthetic file, so the threshold needs real margin below the
    // achievable value, not the unreachable ideal, while staying well above
    // the ~0.001 confidence seen from a misaligned/no-burst window (noise).
    // 0.10, не 0.06: на реальных эфирных записях (data/iq_pluto_*.bin) шум в
    // окне цветовой вспышки даёт устойчивые 0.063 и ложно включает цветовой
    // декод — картинка покрывается зелёно-розовыми разводами, хотя цвета в
    // сигнале нет (проверено по спектру заднего порча: мощность на 3.5795 MHz
    // всего 0.7-0.8 от медианы). Настоящая вспышка даёт 0.135-0.14, так что
    // порог посередине разделяет эти случаи с запасом в обе стороны.
    static constexpr float  kChromaLockThreshold = 0.10f;
    static constexpr float  kChromaLockEmaAlpha  = 0.2f;         // smoothing for the lock-confidence EMA
    static constexpr double kAssumedSyncWidthUs  = 4.7;          // standard NTSC sync width, matches mp4_to_iq.py's
                                                                   // LineTiming.sync_samples — confirmed_sync fires
                                                                   // tv_min_sync_samples_ (4.0us) into that pulse, not
                                                                   // at its start, so the porch/burst window's true
                                                                   // zero point lags tv_line_sample_idx_==0 by the
                                                                   // (sync_width - debounce) remainder; see the burst
                                                                   // window derivation comment in reconfigure_tv_video
    static constexpr double kOutputSampleRateHz    = 48000.0;
    static constexpr std::size_t kWaveformWindow   = 2048;
    static constexpr int    kVsyncStreakThreshold  = 3; // consecutive short sync intervals -> vertical sync
    static constexpr float  kAgcTargetPeak         = 0.7f;   // makeup gain drives recovered audio toward this
    static constexpr float  kAgcMaxGain            = 80.0f;  // cap so a near-silent gap doesn't scream on gain-up
    static constexpr double kAgcReleaseSeconds     = 0.3;    // envelope decay / gain recovery time constant —
                                                               // slow enough not to pump within a single audio
                                                               // cycle or ride up on noise between loud passages

    // shared front end
    std::vector<Complex> channel_iq_;
    std::vector<Complex> decimated_;
    FirDecimator          stage1_decimator_;
    FmDiscriminator        discriminator_;
    std::vector<float>    disc_out_; // also read by run_tv_audio_subcarrier_path() same iteration

    // shared audio tail (FM-audio path output, or TV audio-subcarrier output)
    OnePoleLowpass  deemphasis_;
    FirFilterReal   audio_lpf_;
    LinearResampler resampler_{ 48000.0, kOutputSampleRateHz };
    std::vector<float> deemph_out_;
    std::vector<float> lpf_out_;
    std::vector<float> resampled_;
    std::vector<float> waveform_ring_;

    // automatic gain: recovered FM-audio/subcarrier amplitude varies a lot
    // with source deviation (a weak subcarrier can peak well under 0.05), so
    // a peak-hold envelope (agc_envelope_ = max(|x|, envelope*decay) — never
    // instantly forgets a peak, only leaks down slowly) drives a makeup gain
    // toward a fixed target peak instead of relying on the raw discriminator
    // scale. Gain REDUCTION is applied instantly (never lags a sudden loud
    // sample, which is what causes clipping) while gain RECOVERY after the
    // signal quiets down is smoothed by agc_release_coeff_ so it doesn't
    // pump/ride up on noise between loud passages.
    float agc_envelope_{ 0.01f };
    float agc_gain_{ 1.0f };
    float agc_release_coeff_{ 0.01f }; // also used for the envelope's own decay

    // TV video branch
    FirFilterReal       video_lpf_;
    std::vector<float>  video_lpf_out_;
    bool                tv_luma_narrow_{ false };

    // NTSC color chroma branch — parallel to the luma path above, reading
    // disc_out_ directly (pre-luma-LPF: chroma at 3.58MHz would otherwise be
    // filtered away by video_lpf_'s narrower NTSC cutoff). NCO-mix uses the
    // same rotating-complex-multiply idiom as subcarrier_nco_phase_ below
    // (cos/sin per sample was a measured real-time bottleneck already).
    bool                 chroma_enabled_{ false }; // NTSC && burst window fits the porch; set at reconfigure
    double               chroma_nco_phase_{ 0.0 }; // continuous, never reset except at reconfigure
    std::vector<Complex> chroma_mixed_;            // disc_out_ NCO-mixed to baseband, pre-LPF
    FirDecimator          chroma_lpf_;              // complex, factor=kChromaDecimFactor, kVideoChromaLpfTaps taps
    std::vector<Complex> chroma_lpf_out_;           // decimated: ~1/kChromaDecimFactor the length of video_lpf_out_
    std::vector<Complex> chroma_lpf_out_full_;      // chroma_lpf_out_ held back up to full rate, aligned with video_lpf_out_

    // chroma_mixed_ is undelayed relative to disc_out_, but tv_line_sample_idx_
    // is driven off video_lpf_out_ (delayed by (taps-1)/2 samples) — reading
    // the burst window at the same tv_line_sample_idx_ from chroma_mixed_
    // directly would sample (taps-1)/2 samples too late into the physical
    // porch. This ring applies a matching pure delay to a second copy used
    // only for burst accumulation (the copy feeding chroma_lpf_ for column
    // binning is already self-aligned the same way the luma path is).
    std::size_t           tv_burst_align_delay_{ 0 };
    std::vector<Complex> chroma_burst_delay_buf_;
    std::size_t           chroma_burst_delay_pos_{ 0 };
    std::vector<Complex> chroma_burst_aligned_;

    std::size_t tv_burst_start_samples_{ 0 };
    std::size_t tv_burst_length_samples_{ 0 };
    Complex     tv_burst_accum_{ 0.0f, 0.0f };
    float       tv_line_phase_correction_{ 0.0f };
    bool        tv_line_color_valid_{ false };
    float       tv_chroma_lock_ema_{ 0.0f };
    std::vector<Complex> tv_chroma_accum_; // size tv_frame_width_, reuses tv_col_count_

    // TV audio subcarrier branch — mix disc_out_ down by an NCO at the
    // subcarrier offset, then decimate (FM discriminator sensitivity in
    // radians/sample scales with 1/rate, so discriminating directly at the
    // full intermediate_rate_ makes realistic audio-subcarrier deviations
    // barely register — decimating down first, same as the plain FM-audio
    // path does, is what makes the recovered amplitude usable) before a
    // second, independent FM discriminator
    double               subcarrier_nco_phase_{ 0.0 };
    std::vector<Complex> subcarrier_mixed_;
    FirDecimator          subcarrier_lpf_; // now an actual decimator, not factor=1
    std::vector<Complex> subcarrier_lpf_out_;
    FmDiscriminator        subcarrier_discriminator_;
    std::vector<float>   subcarrier_disc_out_;
    int                  subcarrier_decim_factor_{ 1 };
    double               subcarrier_audio_rate_{ 48000.0 }; // = intermediate_rate_ / subcarrier_decim_factor_

    // TV sync detection + line/frame assembly state
    float       tv_env_max_{ 0.1f };
    float       tv_env_min_{ -0.1f };
    // per-line black-level reference: NTSC's "blacker than sync" spec means
    // sync tip sits below true black (blanking) by design — mp4_to_iq.py
    // encodes sync=-1.0, blanking/black=0.0, white=0.85, the same gap a real
    // broadcast signal has — so anchoring gray directly off the sync-to-white
    // envelope (as if sync tip WERE black) lifts every pixel's black point
    // and washes the whole picture toward gray. Measured each line from the
    // flat porch region before the color burst/active video (see
    // process_video_sample), smoothed across lines since that window is only
    // ~1us wide and noisy on its own.
    static constexpr float kBlackLevelEmaAlpha = 0.05f;
    // Опора чёрного берётся из самого синхроимпульса каждой строки, а не из
    // tv_env_min_: leaky-трекер минимума на реальном эфирном сигнале не
    // удерживает уровень (замерено на data/iq_pluto_*.bin — у записи 18 MSPS
    // постоянная составляющая смещена так, что вершина синхроимпульса лежит
    // на -0.775 против -0.474 у 16 MSPS, и один общий трекер на оба случая не
    // работает). Внутри импульса сигнал стоит на известном уровне, так что
    // достаточно усреднить его же отсчёты.
    static constexpr float kSyncLevelEmaAlpha  = 0.05f;
    // авто-уровень белого: пиковый детектор с медленным спадом (~0.5 c),
    // тянется за самой яркой частью картинки
    static constexpr float kWhiteAutoDecay     = 0.9995f;
    float       tv_black_level_ema_{ 0.0f };
    float       tv_sync_level_ema_{ -1.0f };
    float       tv_sync_accum_{ 0.0f };
    std::size_t tv_sync_accum_count_{ 0 };
    float       tv_white_env_{ 0.1f };
    float       tv_line_black_accum_{ 0.0f };
    std::size_t tv_line_black_count_{ 0 };
    std::size_t tv_sync_low_count_{ 0 };
    std::size_t tv_samples_since_sync_{ 0 };
    int         tv_short_sync_streak_{ 0 };
    bool        tv_vsync_fired_{ false }; // latched once per vsync burst so a run of many
                                           // short intervals (real hardware sends several
                                           // equalizing pulses) can't re-trigger publish_tv_frame()
                                           // multiple times before real video lines resume
    bool        tv_line_active_{ false };
    std::size_t tv_line_sample_idx_{ 0 };
    int         tv_row_{ 0 };

    // Номер строки в кадре считается по её положению во времени относительно
    // кадровой синхронизации, а не инкрементом на каждую пойманную строку:
    // иначе любая непойманная строка сдвигает вверх всё, что ниже неё, и
    // картинка дёргается по вертикали (проверено на записях в data/).
    uint64_t    tv_sample_pos_{ 0 };        // сквозной счётчик отсчётов
    uint64_t    tv_field_start_pos_{ 0 };   // позиция начала текущего поля
    bool        tv_field_started_{ false };
    int         tv_last_written_row_{ -1 };

    // Маховик строчной синхронизации (flywheel): настоящие строчные импульсы
    // приходят строго через период строки, поэтому импульс, пришедший не в
    // ожидаемое окно, отбрасывается как ложный. Без этого тёмные детали
    // изображения дают лишние срабатывания (замерено: до 454 импульсов на
    // поле в 262 строки) и затирают настоящие строки.
    uint64_t    tv_last_line_pos_{ 0 };
    bool        tv_have_line_ref_{ false };
    static constexpr float kLinePhaseTolerance = 0.10f; // в долях строки

    // Кадровая синхронизация распознаётся по группе врезных импульсов
    // (в NTSC их 6, длительность ~27 usec). Старый признак — несколько
    // подряд коротких интервалов — на реальном сигнале не срабатывает
    // вообще (замерено), но именно его даёт синтетика mp4_to_iq.py, поэтому
    // рабочими остаются оба.
    std::size_t tv_serration_min_samples_{ 1 };
    int         tv_serration_count_{ 0 };
    uint64_t    tv_serration_first_pos_{ 0 };
    uint64_t    tv_serration_last_pos_{ 0 };
    std::size_t tv_vbi_lines_{ 20 };        // кадровый гасящий интервал, строк

    // Диагностика: период строки и период поля меряются по самому сигналу и
    // сглаживаются, чтобы UI показывал устойчивые числа, а не дрожь.
    static constexpr float kDiagEmaAlpha = 0.05f;
    float       tv_diag_line_samples_{ 0.0f };
    uint64_t    tv_diag_prev_field_pos_{ 0 };
    float       tv_diag_field_samples_{ 0.0f };
    int         tv_diag_rejected_{ 0 };
    int         tv_diag_field_count_{ 0 };

    // Шумоподавление. Постобработка идёт над копией кадра, а не над
    // tv_working_frame_: рабочий кадр продолжает накапливаться построчно, и
    // подмешивать в него предыдущий было бы обратной связью через саму
    // картинку.
    void repair_bad_lines(std::vector<uint8_t>& frame) const;
    void apply_temporal_nr(std::vector<uint8_t>& frame, float strength);
    std::vector<uint8_t> tv_publish_frame_;
    std::vector<uint8_t> tv_prev_frame_;
    int         tv_frame_width_{ 320 };
    int         tv_frame_height_{ 240 };
    std::size_t tv_nominal_line_samples_{ 1 };
    std::size_t tv_porch_samples_{ 0 };
    std::size_t tv_active_video_samples_{ 1 };
    std::size_t tv_min_sync_samples_{ 1 };
    std::vector<float>   tv_col_accum_;
    std::vector<int>     tv_col_count_;
    std::vector<uint8_t> tv_working_frame_;

    // derived config — reconfigure_{fm_audio,tv_video}() rebuild DSP state
    // whenever these change
    uint32_t   current_sample_rate_{ 0 };
    uint32_t   current_channel_bw_{ 0 };
    TvStandard current_tv_standard_{ TvStandard::NTSC };
    bool       current_deemph_on_{ true };
    DemodMode  current_mode_{ DemodMode::Off };
    double     intermediate_rate_{ 48000.0 };
    int        stage1_factor_{ 1 };
};
