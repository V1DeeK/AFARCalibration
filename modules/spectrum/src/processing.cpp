#include "processing.hpp"
#include "sample.hpp"
#include <iostream>
#include <cmath>
#include <thread>
#include <chrono>
#include <algorithm>

// Радиус вокруг центра спектра затираемый под DC-исключение — переиспользуется
// и на полном разрешении (convert_to_dbm, publish_snr), и на уже прореженном
// для экрана массиве. Затирать нужно оба: downsample_for_display() ниже берёт
// максимум по корзине из десятков сырых бинов на одну точку экрана, поэтому
// сглаживание всего в raw-спектре тонет в соседних непритронутых бинах той же
// корзины — спайк остаётся виден на графике, пока не срезан и после прореживания.
static constexpr int DC_SKIP_RADIUS = 2;

// Линейно затирает [half-radius, half+radius] спектра v, беря опорные точки
// сразу за краями полосы — так провал не выглядит плоской полкой.
static void flatten_dc_band(std::vector<float>& v, int radius) {
    int n = static_cast<int>(v.size());
    int half = n / 2;
    int lo = half - radius;
    int hi = half + radius;
    if (lo - 1 < 0 || hi + 1 >= n) return;

    float left  = v[lo - 1];
    float right = v[hi + 1];
    int   span  = hi - lo + 2;
    for (int k = lo; k <= hi; ++k) {
        float t = static_cast<float>(k - lo + 1) / static_cast<float>(span);
        v[k] = left + (right - left) * t;
    }
}

ProcessingThread::ProcessingThread(CircularBuffer<CoherentSample>& buf, AppState& state)
    : buf_(buf), state_(state) {}

ProcessingThread::~ProcessingThread() {
    if (plan_)      fftwf_destroy_plan(plan_);
    if (fftw_in1_)  fftwf_free(fftw_in1_);
    if (fftw_out1_) fftwf_free(fftw_out1_);
    if (fftw_in2_)  fftwf_free(fftw_in2_);
    if (fftw_out2_) fftwf_free(fftw_out2_);
}

// public

void ProcessingThread::run() {
    std::cout << "[processing] started\n";

    while (state_.running.load()) {
        int requested_size = state_.fft_size.load();

        // rebuild plan whenever fft_size changes (user switched dropdown)
        if (requested_size != fft_size_)
            rebuild_plan(requested_size);

        // wait until a full batch is available
        if (buf_.available() < static_cast<size_t>(fft_size_)) {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            continue;
        }

        if (!buf_.pop_batch(batch_.data(), static_cast<size_t>(fft_size_)))
            continue;

        PlutoChannelMode current_mode = state_.chan_mode.load();
        bool dual = (current_mode == PlutoChannelMode::Both);

        // copy windowed batch into fftw input buffer(s)
        // in Both mode rx1 and rx2 are kept separate (two spectra) instead of averaged
        for (int k = 0; k < fft_size_; ++k) {
            double win = hann_window_[k];
            const Complex& primary = (current_mode == PlutoChannelMode::Channel_2) ? batch_[k].rx2 : batch_[k].rx1;

            fftw_in1_[k][0] = primary.real() * win;
            fftw_in1_[k][1] = primary.imag() * win;

            if (dual) {
                fftw_in2_[k][0] = batch_[k].rx2.real() * win;
                fftw_in2_[k][1] = batch_[k].rx2.imag() * win;
            }
        }

        // transform channel 1 with the plan, channel 2 (if any) via new-array execute on the same plan
        fftwf_execute(plan_);
        if (dual)
            fftwf_execute_dft(plan_, fftw_in2_, fftw_out2_);

        // put those new samples into the welch accumulator(s)
        accumulate_frame(fftw_out1_, welch_accum1_);
        if (dual) accumulate_frame(fftw_out2_, welch_accum2_);

        ++frame_count_;
        // augment frames until desired #
        // when reached, send to display thread
        if (frame_count_ >= WELCH_FRAMES) {
            average_welch(welch_accum1_);
            convert_to_dbm(welch_accum1_, psd_result1_);
            publish_snr(psd_result1_, false);
            downsample_for_display(psd_result1_, display_result1_);
            // re-flatten at display resolution: downsample_for_display's max-hold
            // binning pulls in whichever raw bin in the DC bucket is highest, so
            // the raw-resolution flatten in convert_to_dbm() alone doesn't survive it
            if (state_.exclude_dc_bin.load())
                flatten_dc_band(display_result1_, DC_SKIP_RADIUS);
            publish_channel(display_result1_, state_.psd_dbm, state_.waterfall);

            // read the one-shot reset flag once so both channels reset together;
            // only clear it once actually consumed (peak hold on), same as before —
            // a reset requested while peak hold is off stays pending until it's enabled
            bool peak_reset = state_.peak_hold_reset.load();
            update_peak(display_result1_, state_.peak_dbm, peak_reset);

            if (dual) {
                average_welch(welch_accum2_);
                convert_to_dbm(welch_accum2_, psd_result2_);
                publish_snr(psd_result2_, true);
                downsample_for_display(psd_result2_, display_result2_);
                if (state_.exclude_dc_bin.load())
                    flatten_dc_band(display_result2_, DC_SKIP_RADIUS);
                publish_channel(display_result2_, state_.psd_dbm2, state_.waterfall2);
                update_peak(display_result2_, state_.peak_dbm2, peak_reset);
            }

            // Вне режима Both второго спектра нет, и старые показания SNR по нему
            // остались бы на экране как измерение несуществующего канала.
            if (!dual) state_.snr_valid2.store(false);

            if (peak_reset && state_.peak_hold_on.load()) state_.peak_hold_reset.store(false);

            frame_count_ = 0;
        }
    }

    std::cout << "[processing] exiting\n";
}

// private

void ProcessingThread::rebuild_plan(int new_fft_size) {
    if (plan_)      fftwf_destroy_plan(plan_);
    if (fftw_in1_)  fftwf_free(fftw_in1_);
    if (fftw_out1_) fftwf_free(fftw_out1_);
    if (fftw_in2_)  fftwf_free(fftw_in2_);
    if (fftw_out2_) fftwf_free(fftw_out2_);

    fft_size_ = new_fft_size;

    // fftwf_malloc gives aligned memory — FFTW uses SIMD internally
    fftw_in1_  = reinterpret_cast<fftwf_complex*>(
                    fftwf_malloc(sizeof(fftwf_complex) * fft_size_));
    fftw_out1_ = reinterpret_cast<fftwf_complex*>(
                    fftwf_malloc(sizeof(fftwf_complex) * fft_size_));
    fftw_in2_  = reinterpret_cast<fftwf_complex*>(
                    fftwf_malloc(sizeof(fftwf_complex) * fft_size_));
    fftw_out2_ = reinterpret_cast<fftwf_complex*>(
                    fftwf_malloc(sizeof(fftwf_complex) * fft_size_));

    if (!fftw_in1_ || !fftw_out1_ || !fftw_in2_ || !fftw_out2_) {
        std::cerr << "[processing] fftwf_malloc failed\n";
        state_.running.store(false);
        return;
    }

    // ESTIMATE builds the plan instantly instead of benchmarking codelets; all supported
    // sizes are powers of two, where ESTIMATE already picks a near-optimal radix-2 plan,
    // so MEASURE's benchmarking pass bought negligible speed while costing up to seconds
    // of synchronous stall on this thread whenever the user picked a large size (32768/65536).
    // channel 2 reuses this same plan via fftwf_execute_dft (new-array execute) —
    // both buffer pairs are fftwf_malloc'd so alignment matches
    plan_ = fftwf_plan_dft_1d(fft_size_, fftw_in1_, fftw_out1_,
                               FFTW_FORWARD, FFTW_ESTIMATE);
    if (!plan_) {
        std::cerr << "[processing] fftwf_plan_dft_1d failed\n";
        state_.running.store(false);
        return;
    }

    batch_.resize(fft_size_);
    welch_accum1_.assign(fft_size_, 0.0f);
    welch_accum2_.assign(fft_size_, 0.0f);
    psd_result1_.resize(fft_size_);
    psd_result2_.resize(fft_size_);
    fftshift_scratch_.resize(fft_size_);

    int display_bins = std::min(fft_size_, DISPLAY_BINS);
    display_result1_.resize(display_bins);
    display_result2_.resize(display_bins);

    frame_count_ = 0;

    // precompute Hann window coefficients for this fft_size
    // w[n] = 0.5 * (1 - cos(2π*n / (N-1)))
    hann_window_.resize(fft_size_);
    for (int n = 0; n < fft_size_; ++n)
        hann_window_[n] = 0.5f * (1.0f - cosf(2.0f * PI * n / (fft_size_ - 1)));

    std::cout << "[processing] plan rebuilt for fft_size=" << fft_size_ << "\n";
}

void ProcessingThread::apply_hann_window() {
    // // tapers edges of batch to zero, reducing spectral leakage
    // for (int n = 0; n < fft_size_; ++n)
    //     batch_[n] *= hann_window_[n];

    throw std::logic_error("Functionality not implemented yet.");
}

void ProcessingThread::accumulate_frame(fftwf_complex* fft_out, std::vector<float>& accum) {
    // add |X[k]|^2 from this FFT frame into the running Welch sum
    for (int k = 0; k < fft_size_; ++k) {
        float re = fft_out[k][0];
        float im = fft_out[k][1];
        accum[k] += re * re + im * im;
    }
}

void ProcessingThread::average_welch(std::vector<float>& accum) {
    float frames = static_cast<float>(WELCH_FRAMES);
    for (int k = 0; k < fft_size_; ++k)
        accum[k] /= frames;
}

void ProcessingThread::convert_to_dbm(std::vector<float>& accum, std::vector<float>& result) {
    float cal = state_.cal_offset_db.load();
    float n2  = static_cast<float>(fft_size_) * static_cast<float>(fft_size_);

    for (int k = 0; k < fft_size_; ++k) {
        // power_dBm = 10 * log10(|X[k]|^2 / N^2) + calibration_offset
        // clamp to avoid log10(0) = -inf
        float power = accum[k] / n2;
        result[k] = 10.0f * log10f(power > 1e-20f ? power : 1e-20f) + cal;
    }

    // fftshift: swap halves so DC is centered, negative freqs on the left
    int half = fft_size_ / 2;
    for (int k = 0; k < half; ++k) {
        fftshift_scratch_[k]        = result[k + half];
        fftshift_scratch_[k + half] = result[k];
    }
    std::swap(result, fftshift_scratch_);

    // interpolate the bins around DC to remove the LO-leakage spike
    // (the raw-resolution half of the fix — see flatten_dc_band() and the
    // display-resolution pass applied after downsample_for_display() in run())
    if (state_.exclude_dc_bin.load())
        flatten_dc_band(result, DC_SKIP_RADIUS);

    std::fill(accum.begin(), accum.end(), 0.0f);
}

// Медиана усреднённой периодограммы лежит ниже её среднего: после усреднения по
// WELCH_FRAMES кадрам мощность в бине распределена как chi^2 с 2*WELCH_FRAMES
// степенями свободы, у которого медиана при 8 кадрах на 0.18 дБ ниже среднего.
// Поправка мелкая, но она ровно в ту сторону, где SNR иначе завышается.
static constexpr float MEDIAN_TO_MEAN_DB = 0.18f;

void ProcessingThread::publish_snr(const std::vector<float>& psd, bool second_channel) {
    int n = static_cast<int>(psd.size());

    std::atomic<bool>*   valid = second_channel ? &state_.snr_valid2     : &state_.snr_valid;
    std::atomic<float>*  snr   = second_channel ? &state_.snr_db2        : &state_.snr_db;
    std::atomic<float>*  noise = second_channel ? &state_.snr_noise_dbm2 : &state_.snr_noise_dbm;
    std::atomic<float>*  peak  = second_channel ? &state_.snr_peak_dbm2  : &state_.snr_peak_dbm;
    std::atomic<double>* pfreq = second_channel ? &state_.snr_peak_hz2   : &state_.snr_peak_hz;

    if (n < 16) {
        valid->store(false);
        return;
    }

    // Шумовой пол — медиана по бинам. Среднее сюда не годится: одна сильная
    // несущая тянет его вверх на единицы дБ, и «шумовой пол» начинает зависеть
    // от того, что в эфире. Медиана держится за шум, пока сигналы занимают
    // меньше половины полосы.
    // Считаем по прореженной копии: при 65536 бинах полная nth_element на
    // каждый спектр — это заметная работа, а медиана шума от прореживания
    // раз в 32 бина не меняется.
    int stride = std::max(1, n / 2048);
    snr_scratch_.clear();
    for (int k = 0; k < n; k += stride)
        snr_scratch_.push_back(psd[k]);

    std::size_t mid = snr_scratch_.size() / 2;
    std::nth_element(snr_scratch_.begin(), snr_scratch_.begin() + mid, snr_scratch_.end());
    float noise_db = snr_scratch_[mid] + MEDIAN_TO_MEAN_DB;

    // Пик. Центральные бины пропускаем: на приёмнике прямого преобразования
    // там утечка гетеродина, а сам центральный бин к тому же синтетический —
    // он интерполирован по соседям в convert_to_dbm().
    int   half     = n / 2;
    int   best_bin = -1;
    float best_db  = -1e30f;
    for (int k = 0; k < n; ++k) {
        if (std::abs(k - half) <= DC_SKIP_RADIUS) continue;
        if (psd[k] > best_db) {
            best_db  = psd[k];
            best_bin = k;
        }
    }
    if (best_bin < 0) {
        valid->store(false);
        return;
    }

    double center = static_cast<double>(state_.center_freq_hz.load());
    double rate   = static_cast<double>(state_.sample_rate_hz.load());

    noise->store(noise_db);
    peak->store(best_db);
    pfreq->store(center + (best_bin - half) * rate / static_cast<double>(n));
    snr->store(best_db - noise_db);
    valid->store(true);
}

void ProcessingThread::downsample_for_display(const std::vector<float>& full, std::vector<float>& out) {
    int full_n = static_cast<int>(full.size());
    int out_n  = static_cast<int>(out.size());

    if (out_n >= full_n) {
        out = full;
        return;
    }

    // max-hold binning: keep the strongest bin in each bucket so narrowband
    // peaks/spurs survive the downsample instead of being averaged away
    for (int i = 0; i < out_n; ++i) {
        int lo = static_cast<int>(static_cast<int64_t>(i)     * full_n / out_n);
        int hi = static_cast<int>(static_cast<int64_t>(i + 1) * full_n / out_n);
        hi = std::max(hi, lo + 1);

        float m = full[lo];
        for (int k = lo + 1; k < hi; ++k)
            m = std::max(m, full[k]);
        out[i] = m;
    }
}

void ProcessingThread::publish_channel(const std::vector<float>& result,
                                        std::vector<float>& dst_psd, std::vector<float>& dst_waterfall) {
    std::lock_guard<std::mutex> lock(state_.psd_mutex);

    dst_psd = result;
    state_.total_frames.fetch_add(1);

    // waterfall row width follows the (possibly downsampled) published result size,
    // not fft_size_ directly — must match what render_waterfall_pane assumes (psd_dbm.size())
    int row_width = static_cast<int>(result.size());
    int total = AppState::WATERFALL_ROWS * row_width;
    if (static_cast<int>(dst_waterfall.size()) != total)
        dst_waterfall.assign(total, -120.0f);

    // shift rows down, insert new row at top (row 0 = newest)
    std::move_backward(dst_waterfall.begin(),
                        dst_waterfall.end() - row_width,
                        dst_waterfall.end());
    std::copy(result.begin(), result.end(), dst_waterfall.begin());
}

void ProcessingThread::update_peak(const std::vector<float>& result, std::vector<float>& peak, bool reset) {
    if (!state_.peak_hold_on.load()) return;

    std::lock_guard<std::mutex> lock(state_.psd_mutex);

    // basically on reset, for each frequency bin, set it to -200 dBm which is below the noise floor.
    if (reset) {
        peak.assign(result.size(), -200.0f);
        return;
    }

    // if the peak hold array size doesn't match the current PSD size (because user changed FFT size)
    // resize it and fill with -200 dBm (no peaks).
    if (peak.size() != result.size())
        peak.assign(result.size(), -200.0f);

    // go through each frequency and compare current and "highest"
    for (size_t k = 0; k < result.size(); ++k) {
        if (result[k] > peak[k])
            peak[k] = result[k];
    }
}
