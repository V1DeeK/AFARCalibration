#pragma once

#include "app_state.hpp"
#include "circular_buffer.hpp"
#include "clip_monitor.hpp"
#include "sample.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// Replays a raw interleaved int16 IQ .bin file (same format as
// PlutoCaptureThread::save_iq_file() / python/psd_generate.py /
// python/mp4_to_iq.py) through the same three buffers live capture feeds,
// so PSD/peleng/demod all work unmodified on played-back data. Idle unless
// state_.playback_requested is set — mirrors DemodThread's "sleep when
// inactive" pattern.
//
// Two file layouts, told apart by state_.playback_dual_channel (the file itself
// carries no such marker — see the comment there):
//   single — i,q per sample; rx2 is zeroed
//   dual   — i1,q1,i2,q2 per sample, as recorded in Both mode; the only layout
//            direction finding can do anything with, since monopulse needs both
//            coherent channels
class IqPlaybackThread {
public:
    IqPlaybackThread(CircularBuffer<CoherentSample>& fft_buf,
                      CircularBuffer<CoherentSample>& df_buf,
                      CircularBuffer<CoherentSample>& demod_buf,
                      AppState& state);

    void run();

private:
    void play_file(const std::string& path);
    void stream_once(std::ifstream& f, bool dual);

    // Перегрузка в файле — тот же счётчик и тот же порог, что на живом потоке.
    // Ради этого клиппинг и считается по сырым int16 до нормировки: иначе
    // запись, сделанную с зажатым АЦП, при просмотре было бы не отличить от
    // просто сильного сигнала.
    ClipMonitor                     clip_monitor_;

    CircularBuffer<CoherentSample>& fft_buf_;
    CircularBuffer<CoherentSample>& df_buf_;
    CircularBuffer<CoherentSample>& demod_buf_;
    AppState&                       state_;

    static constexpr std::size_t kChunkSamples = 4096;
};
