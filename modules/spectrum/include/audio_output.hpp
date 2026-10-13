#pragma once

#include <cstdint>

#include "app_state.hpp"
#include "circular_buffer.hpp"

// Forward-declare rather than include miniaudio.h here — keeps the giant
// header out of every translation unit that only needs this class's
// interface, same discipline display.hpp uses for GLFW/ImGui.
struct ma_device;

// Wraps a miniaudio playback device. Pulls PCM from a CircularBuffer<float>
// fed by DemodThread and plays it through the system's default audio output.
// Not itself a std::thread — miniaudio runs its own realtime callback thread
// internally once started.
class AudioOutputDevice {
public:
    AudioOutputDevice(CircularBuffer<float>& audio_out_buf, AppState& state);
    ~AudioOutputDevice();

    AudioOutputDevice(const AudioOutputDevice&)            = delete;
    AudioOutputDevice& operator=(const AudioOutputDevice&) = delete;

    // Opens the default playback device and starts the callback. Returns
    // false on failure (also reflected in state_.audio_device_ok) — no
    // available audio backend is treated as non-fatal, not a crash.
    bool start();

private:
    static void data_callback(ma_device* device, void* output, const void* input, unsigned int frame_count);
    void fill(float* output, unsigned int frame_count);

    CircularBuffer<float>& audio_out_buf_;
    AppState&               state_;

    ma_device* device_{ nullptr };
    uint32_t   sample_rate_hz_{ 48000 };
};
