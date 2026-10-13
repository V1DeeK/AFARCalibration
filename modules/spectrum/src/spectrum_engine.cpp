#include "spectrum_engine.hpp"

#include "audio_output.hpp"
#include "board_finder.hpp"
#include "capture.hpp"
#include "circular_buffer.hpp"
#include "demod.hpp"
#include "finding.hpp"
#include "playback.hpp"
#include "processing.hpp"
#include "tv_recorder.hpp"

#include <iostream>
#include <thread>

struct SpectrumEngine::Runtime {
    explicit Runtime(AppState& state, bool enableAudio)
        : state(state),
          fftBuffer(1 << 18),
          directionBuffer(1 << 18),
          demodBuffer(1 << 23),
          audioBuffer(1 << 16),
          capture(fftBuffer, directionBuffer, demodBuffer, state),
          processing(fftBuffer, state),
          finding(directionBuffer, state),
          demod(demodBuffer, audioBuffer, state),
          audio(audioBuffer, state),
          playback(fftBuffer, directionBuffer, demodBuffer, state),
          tvRecorder(state),
          boardFinder(state),
          enableAudio(enableAudio) {}

    ~Runtime() {
        state.running.store(false);
        joinAll();
    }

    void startAll() {
        captureThread = std::thread([this] { capture.run(); });
        processingThread = std::thread([this] { processing.run(); });
        findingThread = std::thread([this] { finding.run(); });
        demodThread = std::thread([this] { demod.run(); });
        playbackThread = std::thread([this] { playback.run(); });
        tvRecorderThread = std::thread([this] { tvRecorder.run(); });
        boardFinderThread = std::thread([this] { boardFinder.run(); });

        if (enableAudio && !audio.start())
            std::cerr << "[spectrum] audio output unavailable; continuing without audio\n";
    }

    void joinAll() {
        auto join = [](std::thread& thread) {
            if (thread.joinable())
                thread.join();
        };
        join(captureThread);
        join(processingThread);
        join(findingThread);
        join(demodThread);
        join(playbackThread);
        join(tvRecorderThread);
        join(boardFinderThread);
    }

    AppState& state;
    CircularBuffer<CoherentSample> fftBuffer;
    CircularBuffer<CoherentSample> directionBuffer;
    CircularBuffer<CoherentSample> demodBuffer;
    CircularBuffer<float> audioBuffer;

    PlutoCaptureThread capture;
    ProcessingThread processing;
    DirectionFindingThread finding;
    DemodThread demod;
    AudioOutputDevice audio;
    IqPlaybackThread playback;
    TvRecorderThread tvRecorder;
    BoardFinderThread boardFinder;
    bool enableAudio;

    std::thread captureThread;
    std::thread processingThread;
    std::thread findingThread;
    std::thread demodThread;
    std::thread playbackThread;
    std::thread tvRecorderThread;
    std::thread boardFinderThread;
};

SpectrumEngine::SpectrumEngine(bool enableAudio) : enableAudio_(enableAudio) {
    state_.running.store(false);
}

SpectrumEngine::~SpectrumEngine() {
    stopAndJoin();
}

void SpectrumEngine::start() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (runtime_)
        return;

    state_.playback_stop_requested.store(false);
    state_.tv_record_stop_requested.store(false);
    state_.disconnect_requested.store(false);
    state_.peleng_log_stop_requested.store(false);
    state_.running.store(true);

    auto runtime = std::make_unique<Runtime>(state_, enableAudio_);
    try {
        runtime->startAll();
        runtime_ = std::move(runtime);
    } catch (...) {
        state_.running.store(false);
        throw;
    }
}

void SpectrumEngine::requestStop() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    state_.playback_stop_requested.store(true);
    state_.tv_record_stop_requested.store(true);
    state_.disconnect_requested.store(true);
    state_.peleng_log_stop_requested.store(true);
    state_.running.store(false);
}

void SpectrumEngine::stopAndJoin() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    state_.playback_stop_requested.store(true);
    state_.tv_record_stop_requested.store(true);
    state_.disconnect_requested.store(true);
    state_.peleng_log_stop_requested.store(true);
    state_.running.store(false);
    runtime_.reset();
}

bool SpectrumEngine::isRunning() const {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return runtime_ != nullptr && state_.running.load();
}

AppState& SpectrumEngine::state() noexcept {
    return state_;
}

const AppState& SpectrumEngine::state() const noexcept {
    return state_;
}
