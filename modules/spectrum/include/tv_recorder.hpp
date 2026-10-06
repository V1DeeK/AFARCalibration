#pragma once

#include "app_state.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Encodes the decoded TV picture (state_.tv_frame) to an MP4 file by piping
// raw RGB24 frames into an ffmpeg subprocess. Runs as its own thread,
// entirely decoupled from DemodThread (the producer) and DisplayThread (the
// other consumer) — both only ever touch tv_frame_mutex, the same brief lock
// they already used before recording existed. This thread polls
// tv_frame_update_count, copies the latest frame under that lock, then does
// all the potentially slow work (ffmpeg pipe write, which can block on disk
// or encoder backpressure) outside it — so a stalled recording never stalls
// the live picture or the rest of the UI, at worst falling behind and
// skipping to whatever the newest frame is next time it looks.
class TvRecorderThread {
public:
    explicit TvRecorderThread(AppState& state);

    void run();

private:
    bool start_recording(const std::string& path);
    void stop_recording();
    void write_frame(const std::vector<uint8_t>& frame);

    AppState& state_;

    FILE*    ffmpeg_pipe_{ nullptr };
    int      rec_width_{ 0 };
    int      rec_height_{ 0 };
    uint64_t last_seen_update_count_{ 0 };
};
