#pragma once

#include "app_state.hpp"
#include "circular_buffer.hpp"
#include "sample.hpp"
#include <complex>
#include <vector>
#include <fftw3.h>

class ProcessingThread {
public:
    ProcessingThread(CircularBuffer<CoherentSample>& buf, AppState& state);
    ~ProcessingThread();

    void run();

private:
    void rebuild_plan(int new_fft_size);
    void apply_hann_window();
    void accumulate_frame(fftwf_complex* fft_out, std::vector<float>& accum);
    void average_welch(std::vector<float>& accum);    // divide accumulator by frame count
    void convert_to_dbm(std::vector<float>& accum, std::vector<float>& result); // apply formula + fftshift
    void downsample_for_display(const std::vector<float>& full, std::vector<float>& out); // max-hold binning to a bounded bin count
    void publish_channel(const std::vector<float>& result,
                          std::vector<float>& dst_psd, std::vector<float>& dst_waterfall); // lock mutex, write to AppState
    void update_peak(const std::vector<float>& result, std::vector<float>& peak, bool reset);
    // Шумовой пол, самый сильный бин и SNR по полному (не прореженному для
    // экрана) спектру — считается там же, где спектр, потому что прореживание
    // ниже идёт максимумом по корзине и шумовой пол по нему завышен.
    void publish_snr(const std::vector<float>& psd, bool second_channel);

    CircularBuffer<CoherentSample>& buf_;
    AppState&                state_;

    int            fft_size_{ 0 };
    // time domain samples before fft — one pair of buffers per channel (ch2 only used in Both mode)
    fftwf_complex* fftw_in1_ { nullptr };
    fftwf_complex* fftw_out1_{ nullptr };
    fftwf_complex* fftw_in2_ { nullptr };
    fftwf_complex* fftw_out2_{ nullptr };
    // optimized fft configuration — same size/direction for both channels, reused via new-array execute
    fftwf_plan     plan_    { nullptr };

    // batch popped from the ring buffer each iteration
    std::vector<CoherentSample> batch_;

    std::vector<float> hann_window_;
    std::vector<float> welch_accum1_;
    std::vector<float> welch_accum2_;
    std::vector<float> psd_result1_;
    std::vector<float> psd_result2_;
    std::vector<float> fftshift_scratch_; // reused fftshift buffer for convert_to_dbm, avoids a per-call allocation
    std::vector<float> snr_scratch_;      // прореженная копия спектра под nth_element (медиана переставляет элементы)

    // published/rendered PSD is capped at DISPLAY_BINS regardless of fft_size — the PSD/waterfall
    // panes are at most ~1400px wide, so pushing all 65536 bins through the mutex-held copy,
    // waterfall row-shift, and heatmap/line rendering every frame is wasted work
    static constexpr int DISPLAY_BINS = 2048;
    std::vector<float> display_result1_;
    std::vector<float> display_result2_;

    static constexpr int WELCH_FRAMES = 8;
    int frame_count_{ 0 };
};
