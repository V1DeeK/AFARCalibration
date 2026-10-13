#pragma once

// Small reusable DSP building blocks for the demodulation pipeline
// (DemodThread): FIR design, a decimating complex FIR, a real-valued FIR,
// an FM discriminator, and a fractional-ratio linear resampler. Nothing
// here existed before — PSD/peleng both operate at the full capture rate.

#include <complex>
#include <cstddef>
#include <vector>

#include "app_state.hpp" // Complex

// Windowed-sinc low-pass FIR design (Hamming window), unity DC gain.
// cutoff_norm is the cutoff frequency as a fraction of Nyquist (0 < x < 1).
// num_taps is forced to the next odd value if even (Type-I linear phase).
std::vector<float> design_lowpass_fir(int num_taps, float cutoff_norm);

// Direct-form FIR decimator for complex input: applies the (anti-alias)
// low-pass FIR and keeps every `factor`-th filtered sample.
class FirDecimator {
public:
    FirDecimator() : FirDecimator(std::vector<float>{ 1.0f }, 1) {} // passthrough until reconfigure() is called
    FirDecimator(std::vector<float> taps, int factor);

    void reconfigure(std::vector<float> taps, int factor);

    // Appends decimated output to `out` (out is not cleared first).
    void process(const Complex* in, std::size_t count, std::vector<Complex>& out);

private:
    // taps_rev_[j] = original_taps[n-1-j], and delay_re_/delay_im_ are each
    // double-length (each sample written at both head_ and head_+n) so the
    // last n samples are always a *contiguous* oldest->newest slice starting
    // at head_ — no per-tap modulo/branch in the hot inner loop, letting the
    // compiler auto-vectorize the dot product. This runs at the raw
    // (undecimated) sample rate for a factor-1 decimator, so the branchy
    // version was costly enough to keep TV video mode from sustaining
    // real-time at typical capture rates.
    //
    // Real/imaginary channels are kept as separate float arrays (SoA)
    // instead of a std::complex<float> delay line (AoS, re/im interleaved)
    // — measured with -fopt-info-vec-all: GCC's auto-vectorizer gives up on
    // the interleaved std::complex dot product ("more than one data ref in
    // stmt", 0 loops vectorized), but happily vectorizes two independent
    // plain-float reduction loops the same way it already does for
    // FirFilterReal below. Taps are real (design_lowpass_fir's output), so
    // each output channel is just a real dot product against the matching
    // delay channel — no complex multiply needed at all.
    std::vector<float> taps_rev_;
    std::vector<float> delay_re_;
    std::vector<float> delay_im_;
    std::size_t          n_{ 0 };
    std::size_t          head_{ 0 };
    int                  factor_{ 1 };
    int                  phase_{ 0 };
};

// Direct-form FIR filter (no decimation) for real-valued input. Meant to run
// at an already-decimated rate (audio-ish, not raw IQ rate) since it's O(N)
// per input sample.
class FirFilterReal {
public:
    FirFilterReal() : FirFilterReal(std::vector<float>{ 1.0f }) {} // passthrough until reconfigure() is called
    explicit FirFilterReal(std::vector<float> taps);
    void reconfigure(std::vector<float> taps);
    void process(const float* in, std::size_t count, std::vector<float>& out);

private:
    // same double-length-buffer + reversed-taps layout as FirDecimator —
    // see the comment there
    std::vector<float> taps_rev_;
    std::vector<float> delay_;
    std::size_t         n_{ 0 };
    std::size_t         head_{ 0 };
};

// Single-pole IIR low-pass — used for de-emphasis.
class OnePoleLowpass {
public:
    explicit OnePoleLowpass(float alpha = 0.0f) : alpha_(alpha) {}
    void  set_alpha(float alpha) { alpha_ = alpha; }
    float process(float x) { y_ += (x - y_) * alpha_; return y_; }
    void  reset() { y_ = 0.0f; }

private:
    float alpha_{ 0.0f };
    float y_{ 0.0f };
};

// FM discriminator: arg(x[n] * conj(x[n-1])), radians/sample. Carries the
// previous sample across calls so chunk boundaries don't glitch.
class FmDiscriminator {
public:
    void process(const Complex* in, std::size_t count, std::vector<float>& out);

private:
    Complex prev_{ 1.0f, 0.0f };
};

// Fractional-ratio linear-interpolation resampler: arbitrary in_rate to
// arbitrary out_rate (used for the final decimated-audio -> 48kHz step,
// since sample_rate_hz is user-adjustable and won't cleanly divide down).
// Not brick-wall — callers should already have band-limited the input
// before resampling down.
class LinearResampler {
public:
    LinearResampler(double in_rate, double out_rate);
    void set_rates(double in_rate, double out_rate);

    // Appends resampled output to `out`.
    void process(const float* in, std::size_t count, std::vector<float>& out);

private:
    double step_{ 1.0 };
    double phase_{ 0.0 };
    float  prev_{ 0.0f };
    bool   has_prev_{ false };
};
