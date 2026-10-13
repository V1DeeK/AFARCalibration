#pragma once

#include "app_state.hpp"

#include <memory>
#include <mutex>

class SpectrumEngine {
public:
    explicit SpectrumEngine(bool enableAudio = true);
    ~SpectrumEngine();

    SpectrumEngine(const SpectrumEngine&) = delete;
    SpectrumEngine& operator=(const SpectrumEngine&) = delete;

    void start();
    void requestStop();
    void stopAndJoin();

    bool isRunning() const;
    AppState& state() noexcept;
    const AppState& state() const noexcept;

private:
    struct Runtime;

    const bool enableAudio_;
    AppState state_;
    std::unique_ptr<Runtime> runtime_;
    mutable std::mutex lifecycleMutex_;
};
