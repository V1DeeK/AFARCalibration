#include "dsp_utils.hpp"

#include <cmath>

std::vector<float> design_lowpass_fir(int num_taps, float cutoff_norm) {
    if (num_taps < 1) return {};
    if (num_taps % 2 == 0) ++num_taps; // force odd length: Type-I linear phase

    std::vector<float> taps(static_cast<std::size_t>(num_taps));
    float fc = cutoff_norm * 0.5f; // cutoff as a fraction of the sample rate
    int   M  = num_taps - 1;
    float sum = 0.0f;

    for (int n = 0; n < num_taps; ++n) {
        float m = static_cast<float>(n) - static_cast<float>(M) / 2.0f;
        float sinc;
        if (m == 0.0f) {
            sinc = 2.0f * fc;
        } else {
            sinc = std::sin(2.0f * PI * fc * m) / (PI * m);
        }
        float window = 0.54f - 0.46f * std::cos(2.0f * PI * static_cast<float>(n) / static_cast<float>(M));
        taps[static_cast<std::size_t>(n)] = sinc * window;
        sum += taps[static_cast<std::size_t>(n)];
    }

    if (sum != 0.0f) {
        for (auto& t : taps) t /= sum;
    }
    return taps;
}

// ============================================================================
// FirDecimator
// ============================================================================

FirDecimator::FirDecimator(std::vector<float> taps, int factor) {
    reconfigure(std::move(taps), factor);
}

void FirDecimator::reconfigure(std::vector<float> taps, int factor) {
    n_ = taps.size();
    taps_rev_.resize(n_);
    for (std::size_t k = 0; k < n_; ++k)
        taps_rev_[n_ - 1 - k] = taps[k];
    delay_re_.assign(n_ * 2, 0.0f);
    delay_im_.assign(n_ * 2, 0.0f);
    head_   = 0;
    factor_ = factor > 0 ? factor : 1;
    phase_  = 0;
}

void FirDecimator::process(const Complex* in, std::size_t count, std::vector<Complex>& out) {
    if (n_ == 0) return;

    float* const __restrict dre = delay_re_.data();
    float* const __restrict dim = delay_im_.data();
    const float* const __restrict taps = taps_rev_.data();
    const std::size_t n = n_;

    for (std::size_t i = 0; i < count; ++i) {
        float re = in[i].real();
        float im = in[i].imag();
        dre[head_]     = re;
        dre[head_ + n] = re;
        dim[head_]     = im;
        dim[head_ + n] = im;
        head_ = (head_ + 1 == n) ? 0 : head_ + 1;

        if (++phase_ >= factor_) {
            phase_ = 0;
            const float* const __restrict recent_re = dre + head_; // contiguous, oldest..newest
            const float* const __restrict recent_im = dim + head_;
            float y_re = 0.0f, y_im = 0.0f;
            for (std::size_t k = 0; k < n; ++k) {
                y_re += taps[k] * recent_re[k];
                y_im += taps[k] * recent_im[k];
            }
            out.emplace_back(y_re, y_im);
        }
    }
}

// ============================================================================
// FirFilterReal
// ============================================================================

FirFilterReal::FirFilterReal(std::vector<float> taps) { reconfigure(std::move(taps)); }

void FirFilterReal::reconfigure(std::vector<float> taps) {
    n_ = taps.size();
    taps_rev_.resize(n_);
    for (std::size_t k = 0; k < n_; ++k)
        taps_rev_[n_ - 1 - k] = taps[k];
    delay_.assign(n_ * 2, 0.0f);
    head_ = 0;
}

void FirFilterReal::process(const float* in, std::size_t count, std::vector<float>& out) {
    if (n_ == 0) {
        out.insert(out.end(), in, in + count);
        return;
    }

    float* const __restrict delay = delay_.data();
    const float* const __restrict taps = taps_rev_.data();
    const std::size_t n = n_;

    for (std::size_t i = 0; i < count; ++i) {
        delay[head_]     = in[i];
        delay[head_ + n] = in[i];
        head_ = (head_ + 1 == n) ? 0 : head_ + 1;

        const float* const __restrict recent = delay + head_; // contiguous, oldest..newest
        float y = 0.0f;
        for (std::size_t k = 0; k < n; ++k)
            y += taps[k] * recent[k];
        out.push_back(y);
    }
}

// ============================================================================
// FmDiscriminator
// ============================================================================

void FmDiscriminator::process(const Complex* in, std::size_t count, std::vector<float>& out) {
    for (std::size_t i = 0; i < count; ++i) {
        Complex x = in[i];
        Complex prod = x * std::conj(prev_);
        out.push_back(std::arg(prod));
        prev_ = x;
    }
}

// ============================================================================
// LinearResampler
// ============================================================================

LinearResampler::LinearResampler(double in_rate, double out_rate) { set_rates(in_rate, out_rate); }

void LinearResampler::set_rates(double in_rate, double out_rate) {
    step_     = (out_rate > 0.0) ? in_rate / out_rate : 1.0;
    phase_    = 0.0;
    has_prev_ = false;
}

void LinearResampler::process(const float* in, std::size_t count, std::vector<float>& out) {
    if (count == 0) return;
    if (!has_prev_) {
        prev_     = in[0];
        has_prev_ = true;
    }

    double t = phase_;
    while (t < static_cast<double>(count)) {
        std::size_t idx  = static_cast<std::size_t>(t);
        float       frac = static_cast<float>(t - static_cast<double>(idx));
        float       left  = (idx == 0) ? prev_ : in[idx - 1];
        float       right = in[idx];
        out.push_back(left + frac * (right - left));
        t += step_;
    }

    phase_ = t - static_cast<double>(count);
    prev_  = in[count - 1];
}
