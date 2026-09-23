#pragma once

#include <string>

namespace afar {

/// Состояния автомата т. 8.4 (CORE-002).
enum class RunState {
    Idle,
    Connecting,
    SelfTest,
    Ready,
    Running,
    Pausing,
    Paused,
    Stopping,
    Aborted,
    Finalizing,
    Complete,
    Error,
    Recovery,
};

[[nodiscard]] const char* toString(RunState state);

/// Автомат с только допустимыми переходами т. 8.4; иначе false без side effects.
class RunStateMachine {
public:
    [[nodiscard]] RunState state() const noexcept { return state_; }

    /// Попытка перехода. Недопустимый — false, состояние не меняется.
    bool tryTransition(RunState target);

    static bool isAllowed(RunState from, RunState to) noexcept;
    static bool isActiveForError(RunState s) noexcept;

private:
    RunState state_{RunState::Idle};
};

}  // namespace afar
