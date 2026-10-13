#include "tv_recorder.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

TvRecorderThread::TvRecorderThread(AppState& state) : state_(state) {}

void TvRecorderThread::run() {
    std::cout << "[tv_recorder] started\n";

    while (state_.running.load()) {
        if (!state_.tv_is_recording.load()) {
            if (!state_.tv_record_requested.exchange(false)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }

            std::string path;
            {
                std::lock_guard<std::mutex> lock(state_.tv_record_mutex);
                path = state_.tv_record_file_path;
            }

            if (start_recording(path)) {
                state_.tv_record_frames_written.store(0);
                state_.tv_record_error.store(false);
                state_.tv_is_recording.store(true);
            } else {
                state_.tv_record_error.store(true);
            }
            continue;
        }

        if (state_.tv_record_stop_requested.exchange(false)) {
            stop_recording();
            state_.tv_is_recording.store(false);
            continue;
        }

        uint64_t update_count = state_.tv_frame_update_count.load();
        if (update_count == last_seen_update_count_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        last_seen_update_count_ = update_count;

        std::vector<uint8_t> frame;
        int w, h;
        {
            std::lock_guard<std::mutex> lock(state_.tv_frame_mutex);
            frame = state_.tv_frame;
            w     = state_.tv_frame_width;
            h     = state_.tv_frame_height;
        }

        if (w != rec_width_ || h != rec_height_) {
            // standard (NTSC/PAL) switched mid-recording, so the frame no
            // longer matches the dimensions ffmpeg was opened with — finalize
            // what's already been written rather than feed it a mismatched
            // buffer size and corrupt the stream
            std::cerr << "[tv_recorder] frame size changed (" << rec_width_ << "x" << rec_height_
                       << " -> " << w << "x" << h << ") — stopping recording\n";
            stop_recording();
            state_.tv_is_recording.store(false);
            state_.tv_record_error.store(true);
            continue;
        }

        write_frame(frame);
    }

    if (state_.tv_is_recording.load()) {
        stop_recording();
        state_.tv_is_recording.store(false);
    }

    std::cout << "[tv_recorder] exiting\n";
}

bool TvRecorderThread::start_recording(const std::string& path) {
    int w, h;
    {
        std::lock_guard<std::mutex> lock(state_.tv_frame_mutex);
        w = state_.tv_frame_width;
        h = state_.tv_frame_height;
    }
    if (w <= 0 || h <= 0)
        return false;

    // the decoder's line/frame state machine (see demod.cpp's
    // publish_tv_frame) doesn't measure a true instantaneous fps, so encode
    // at the broadcast standard's nominal rate rather than trying to infer
    // one from frame arrival timing
    bool        is_ntsc   = (state_.tv_standard.load() == TvStandard::NTSC);
    const char* framerate = is_ntsc ? "30000/1001" : "25";

    // reject a path that could break out of the double-quoted ffmpeg
    // argument below instead of trying to escape it — this is free-text
    // from a UI field, not attacker-controlled, but a stray quote should
    // fail loudly rather than silently reinterpret the command line
    if (path.empty() || path.find('"') != std::string::npos) {
        std::cerr << "[tv_recorder] invalid output path: \"" << path << "\"\n";
        return false;
    }

    // the default path is relative ("data/…"), so it resolves against
    // whatever directory the binary was launched from — usually build/,
    // which has no data/. ffmpeg would exit immediately with "No such file
    // or directory" and the first frame write would then hit a dead pipe.
    std::error_code ec;
    auto            parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !std::filesystem::create_directories(parent, ec) && ec) {
        std::cerr << "[tv_recorder] cannot create output directory \"" << parent.string()
                   << "\": " << ec.message() << "\n";
        return false;
    }

    std::string cmd = "ffmpeg -y -loglevel error "
                       "-f rawvideo -pixel_format rgb24 -video_size " + std::to_string(w) + "x" + std::to_string(h) +
                       " -framerate " + framerate + " -i - "
                       "-pix_fmt yuv420p -c:v libx264 -preset veryfast -movflags +faststart "
                       "\"" + path + "\"";

    ffmpeg_pipe_ = popen(cmd.c_str(), "w");
    if (!ffmpeg_pipe_) {
        std::cerr << "[tv_recorder] failed to launch ffmpeg\n";
        return false;
    }

    rec_width_               = w;
    rec_height_              = h;
    last_seen_update_count_  = state_.tv_frame_update_count.load();
    std::cout << "[tv_recorder] recording " << w << "x" << h << " @ " << framerate << " -> " << path << "\n";
    return true;
}

void TvRecorderThread::stop_recording() {
    if (ffmpeg_pipe_) {
        pclose(ffmpeg_pipe_); // flushes the pipe and waits for ffmpeg to finalize the mp4 (moov atom etc.) —
                               // blocks this thread only, never the caller of Stop
        ffmpeg_pipe_ = nullptr;
        std::cout << "[tv_recorder] stopped, " << state_.tv_record_frames_written.load() << " frames written\n";
    }
    rec_width_  = 0;
    rec_height_ = 0;
}

void TvRecorderThread::write_frame(const std::vector<uint8_t>& frame) {
    if (!ffmpeg_pipe_) return;

    std::size_t written = std::fwrite(frame.data(), 1, frame.size(), ffmpeg_pipe_);
    if (written != frame.size() || std::ferror(ffmpeg_pipe_)) {
        std::cerr << "[tv_recorder] pipe write failed — stopping recording\n";
        stop_recording();
        state_.tv_is_recording.store(false);
        state_.tv_record_error.store(true);
        return;
    }

    state_.tv_record_frames_written.fetch_add(1);
}
