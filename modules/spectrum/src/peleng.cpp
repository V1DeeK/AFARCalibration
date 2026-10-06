#include "peleng.hpp"

#include <algorithm>
#include <cmath>

// ============================================================================
// SubbandAccumulator — PL: DC removal, окно, БПФ, Sigma/Delta, накопление по
// подполосам, ping-pong буфер (разделы 4-14 PDF)
// ============================================================================

SubbandAccumulator::SubbandAccumulator(const Params& params)
    : params_(params),
      num_subbands_((params.fft_size + params.subband_size - 1) / params.subband_size),
      dc_alpha_(1.0f / static_cast<float>(1 << params.dc_shift_bits)) {
    int n = params_.fft_size;

    in1_  = reinterpret_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * n));
    out1_ = reinterpret_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * n));
    in2_  = reinterpret_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * n));
    out2_ = reinterpret_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * n));

    plan_ = fftwf_plan_dft_1d(n, in1_, out1_, FFTW_FORWARD, FFTW_ESTIMATE);

    // окно Ханна: w[n] = 0.5 * (1 - cos(2*pi*n / (N-1))), раздел 7 PDF
    hann_.resize(n);
    for (int i = 0; i < n; ++i)
        hann_[i] = 0.5f * (1.0f - std::cos(2.0f * PI * static_cast<float>(i) / static_cast<float>(n - 1)));

    accum_.assign(num_subbands_, SubbandStats{});
}

SubbandAccumulator::~SubbandAccumulator() {
    if (plan_) fftwf_destroy_plan(plan_);
    if (in1_)  fftwf_free(in1_);
    if (out1_) fftwf_free(out1_);
    if (in2_)  fftwf_free(in2_);
    if (out2_) fftwf_free(out2_);
}

void SubbandAccumulator::push_frame(const Complex* rx1, const Complex* rx2) {
    int n = params_.fft_size;

    for (int i = 0; i < n; ++i) {
        // IIR-оценка DC, непрерывная между кадрами: mu[k] = mu[k-1] + (x[k]-mu[k-1])/2^N
        dc1_ += (rx1[i] - dc1_) * dc_alpha_;
        dc2_ += (rx2[i] - dc2_) * dc_alpha_;

        Complex y1 = rx1[i] - dc1_;
        Complex y2 = rx2[i] - dc2_;

        float w = hann_[i];
        in1_[i][0] = y1.real() * w;
        in1_[i][1] = y1.imag() * w;
        in2_[i][0] = y2.real() * w;
        in2_[i][1] = y2.imag() * w;
    }

    fftwf_execute(plan_);                       // in1_ -> out1_
    fftwf_execute_dft(plan_, in2_, out2_);       // in2_ -> out2_, тот же план

    for (int k = 0; k < n; ++k) {
        Complex x1(out1_[k][0], out1_[k][1]);
        Complex x2(out2_[k][0], out2_[k][1]);

        // C2[k] — перекос второго канала. Снимается здесь, до Sigma/Delta и до
        // накопления S11/S22/S12, поэтому калиброванными выходят все статистики
        // сразу, а не только моноимпульсное отношение.
        if (!c2_.empty()) x2 *= c2_[k];

        // калибровка "нулевого положения луча": alpha_left = 1+0j для v1
        Complex left_cal = alpha_left_ * x1;
        Complex sigma = left_cal + x2;
        Complex delta = left_cal - x2;

        int p = std::min(k / params_.subband_size, num_subbands_ - 1);
        SubbandStats& s = accum_[p];

        s.sum_pow  += std::norm(sigma);
        s.diff_pow += std::norm(delta);

        Complex diff_sum = delta * std::conj(sigma);
        s.diff_sum_re += diff_sum.real();
        s.diff_sum_im += diff_sum.imag();

        s.s11 += std::norm(x1);
        s.s22 += std::norm(x2);

        Complex cross12 = x1 * std::conj(x2);
        s.s12_re += cross12.real();
        s.s12_im += cross12.imag();
    }

    if (++frame_in_accum_ >= params_.accum_frames) {
        ready_ = std::move(accum_);
        accum_.assign(num_subbands_, SubbandStats{});
        ready_flag_ = true;
        frame_in_accum_ = 0;
    }
}

void SubbandAccumulator::set_channel_correction(const std::vector<Complex>& c2_per_bin) {
    // Вектор неправильной длины — это рассогласование геометрии, а не повод
    // молча откалибровать часть полосы: такую поправку надёжнее не применять.
    if (c2_per_bin.size() != static_cast<size_t>(params_.fft_size)) {
        c2_.clear();
        return;
    }
    c2_ = c2_per_bin;
}

std::vector<SubbandStats> SubbandAccumulator::take_ready() {
    ready_flag_ = false;
    return std::move(ready_);
}

// ============================================================================
// SubbandEstimator — PS: ρ[p], γ²[p], SNR[p] по накопленным статистикам
// (раздел 19.4-19.7, 19.11 PDF)
// ============================================================================

// Sigma/Delta, пересчитанные с калибровкой канала 2, из полной ковариации.
// Работает только потому, что вместе с Sigma/Delta передаётся ещё и S11/S22/S12:
// по ним любая линейная комбинация каналов восстанавливается точно, и менять
// прошивку PL ради калибровки не требуется.
static std::vector<SubbandStats> recalibrate(const std::vector<SubbandStats>& stats,
                                              const std::vector<Complex>& c2) {
    std::vector<SubbandStats> out(stats.size());
    for (size_t p = 0; p < stats.size(); ++p) {
        const SubbandStats& s = stats[p];
        SubbandStats& d = out[p];

        double  a2 = static_cast<double>(std::norm(c2[p]));
        // S12 накоплено как <x1 * conj(x2)>, поэтому множителем идёт conj(c2).
        Complex t  = std::conj(c2[p]) * Complex(static_cast<float>(s.s12_re),
                                                 static_cast<float>(s.s12_im));
        double  tr = static_cast<double>(t.real());
        double  ti = static_cast<double>(t.imag());
        double  s22 = a2 * s.s22;

        d.sum_pow     = s.s11 + s22 + 2.0 * tr;
        d.diff_pow    = s.s11 + s22 - 2.0 * tr;
        d.diff_sum_re = s.s11 - s22;
        d.diff_sum_im = 2.0 * ti;
        // Автоспектры и взаимный спектр — тоже калиброванные, иначе γ² и σ в
        // журнале описывали бы не тот сигнал, из которого посчитано ρ.
        d.s11    = s.s11;
        d.s22    = s22;
        d.s12_re = tr;
        d.s12_im = ti;
    }
    return out;
}

std::vector<SubbandResult> SubbandEstimator::estimate(const std::vector<SubbandStats>& stats,
                                                        int accum_frames, int subband_size,
                                                        int fft_size, double sample_rate_hz,
                                                        const std::vector<Complex>& c2_per_subband) const {
    constexpr double eps = 1e-12;
    size_t count = stats.size();
    std::vector<SubbandResult> out(count);
    if (count == 0) return out;

    // Калибровка применяется один раз, до всего остального: локальная медиана
    // шума ниже смотрит на соседние подполосы и должна видеть те же значения,
    // по которым посчитаны ρ и SNR.
    std::vector<SubbandStats>        calibrated;
    const std::vector<SubbandStats>* src = &stats;
    if (c2_per_subband.size() == count) {
        calibrated = recalibrate(stats, c2_per_subband);
        src = &calibrated;
    }

    double samples_per_subband = static_cast<double>(subband_size) * static_cast<double>(accum_frames);
    size_t half_win = static_cast<size_t>(std::max(1, params_.noise_window));
    std::vector<double> window;

    for (size_t p = 0; p < count; ++p) {
        const SubbandStats& s = (*src)[p];
        SubbandResult& r = out[p];
        r.index = static_cast<int>(p);

        int k_center = static_cast<int>(p) * subband_size + subband_size / 2;
        int k_signed = (k_center <= fft_size / 2) ? k_center : k_center - fft_size;
        r.freq_offset_hz = static_cast<float>(k_signed * sample_rate_hz / fft_size);

        double sigma_pow = s.sum_pow + eps;
        r.rho_re = static_cast<float>(s.diff_sum_re / sigma_pow);
        r.rho_im = static_cast<float>(s.diff_sum_im / sigma_pow);

        double cross_mag2 = s.s12_re * s.s12_re + s.s12_im * s.s12_im;
        r.gamma2 = static_cast<float>(cross_mag2 / ((s.s11 + eps) * (s.s22 + eps)));

        // N_p = локальная медиана S_ΣΣ по соседним подполосам Ω(p) (раздел 19.7,
        // второй подход) — устойчивее к смещению вверх на широкополосном сигнале,
        // чем глобальная медиана по всем подполосам
        size_t lo = (p >= half_win) ? p - half_win : 0;
        size_t hi = std::min(count - 1, p + half_win);
        window.assign(hi - lo + 1, 0.0);
        for (size_t q = lo; q <= hi; ++q) window[q - lo] = (*src)[q].sum_pow;
        std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
        double n_p = window[window.size() / 2];

        // SNR_p = (S_ΣΣ[p]-N_p) / (N_p+eps), переведено в дБ (раздел 19.7)
        double snr_ratio = (s.sum_pow - n_p) / std::max(n_p, eps);
        r.snr_db = static_cast<float>(10.0 * std::log10(std::max(snr_ratio, eps)));

        r.sigma_dbfs = static_cast<float>(10.0 * std::log10(s.sum_pow / std::max(samples_per_subband, 1.0) + eps));

        // q_p: маска пригодности (раздел 19.11) — S_ΣΣ>P_min, SNR>SNR_min, γ²>γ²_min
        // (e_p/s_p не проверяются — нет калибровочной таблицы ρcal(θ,f))
        r.valid = (s.sum_pow > params_.min_power) &&
                  (r.snr_db > params_.snr_min_db) &&
                  (r.gamma2 > params_.gamma2_min);
    }

    return out;
}
