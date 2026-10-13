#include "audio_output.hpp"

// Sole translation unit that owns miniaudio's implementation — same
// single-TU discipline as any STB-style header library.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <vector>

namespace {
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels   = 1;
}

AudioOutputDevice::AudioOutputDevice(CircularBuffer<float>& audio_out_buf, AppState& state)
    : audio_out_buf_(audio_out_buf), state_(state), sample_rate_hz_(kSampleRate) {}

AudioOutputDevice::~AudioOutputDevice() {
    if (device_) {
        ma_device_uninit(device_);
        delete device_;
    }
}

void AudioOutputDevice::data_callback(ma_device* device, void* output, const void* /*input*/,
                                       unsigned int frame_count) {
    auto* self = static_cast<AudioOutputDevice*>(device->pUserData);
    self->fill(static_cast<float*>(output), frame_count);
}

void AudioOutputDevice::fill(float* output, unsigned int frame_count) {
    bool  muted  = state_.audio_mute.load();
    float volume = state_.audio_volume.load();

    std::size_t needed = frame_count; // mono
    std::size_t avail  = audio_out_buf_.available();
    std::size_t to_pop = std::min(needed, avail);

    std::vector<float> scratch(to_pop);
    if (to_pop > 0) {
        audio_out_buf_.pop_batch(scratch.data(), to_pop);
    }

    for (std::size_t i = 0; i < needed; ++i) {
        float s   = (i < to_pop) ? scratch[i] : 0.0f;
        output[i] = muted ? 0.0f : s * volume;
    }
}

bool AudioOutputDevice::start() {
    device_ = new ma_device();

    ma_device_config config    = ma_device_config_init(ma_device_type_playback);
    config.playback.format     = ma_format_f32;
    config.playback.channels   = kChannels;
    config.sampleRate          = kSampleRate;
    config.dataCallback        = &AudioOutputDevice::data_callback;
    config.pUserData           = this;

    if (ma_device_init(nullptr, &config, device_) != MA_SUCCESS) {
        delete device_;
        device_ = nullptr;
        state_.audio_device_ok.store(false);
        return false;
    }

    if (ma_device_start(device_) != MA_SUCCESS) {
        ma_device_uninit(device_);
        delete device_;
        device_ = nullptr;
        state_.audio_device_ok.store(false);
        return false;
    }

    sample_rate_hz_ = config.sampleRate;
    state_.audio_device_ok.store(true);
    return true;
}
