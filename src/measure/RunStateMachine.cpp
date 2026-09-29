#include "RunStateMachine.h"

namespace afar {

const char* toString(RunState state)
{
    switch (state) {
    case RunState::Idle:
        return "Idle";
    case RunState::Connecting:
        return "Connecting";
    case RunState::SelfTest:
        return "SelfTest";
    case RunState::Ready:
        return "Ready";
    case RunState::Running:
        return "Running";
    case RunState::Pausing:
        return "Pausing";
    case RunState::Paused:
        return "Paused";
    case RunState::Stopping:
        return "Stopping";
    case RunState::Aborted:
        return "Aborted";
    case RunState::Finalizing:
        return "Finalizing";
    case RunState::Complete:
        return "Complete";
    case RunState::Error:
        return "Error";
    case RunState::Recovery:
        return "Recovery";
    }
    return "Unknown";
}

bool RunStateMachine::isActiveForError(RunState s) noexcept
{
    return s != RunState::Idle && s != RunState::Complete && s != RunState::Aborted;
}

bool RunStateMachine::isAllowed(RunState from, RunState to) noexcept
{
    if (from == to) {
        return false;
    }
    switch (from) {
    case RunState::Idle:
        return to == RunState::Connecting;
    case RunState::Connecting:
        return to == RunState::SelfTest || to == RunState::Error;
    case RunState::SelfTest:
        return to == RunState::Ready || to == RunState::Error;
    case RunState::Ready:
        return to == RunState::Running || to == RunState::Error;
    case RunState::Running:
        return to == RunState::Pausing || to == RunState::Stopping
            || to == RunState::Finalizing || to == RunState::Error;
    case RunState::Pausing:
        return to == RunState::Paused || to == RunState::Error;
    case RunState::Paused:
        return to == RunState::Running || to == RunState::Error;
    case RunState::Stopping:
        return to == RunState::Aborted || to == RunState::Error;
    case RunState::Finalizing:
        return to == RunState::Complete || to == RunState::Error;
    case RunState::Complete:
        // т. 8.4: из Complete исходящих рёбер нет (манифест пишется в Finalizing).
        return false;
    case RunState::Error:
        return to == RunState::Recovery || to == RunState::Aborted;
    case RunState::Aborted:
    case RunState::Recovery:
        return false;
    }
    return false;
}

bool RunStateMachine::tryTransition(RunState target)
{
    if (!isAllowed(state_, target)) {
        return false;
    }
    state_ = target;
    return true;
}

}  // namespace afar
