#include "DutSimulator.h"

#include <string>

void DutSimulator::ensure_connected(const char* op) const
{
    if (!connected_) {
        throw std::runtime_error(
            std::string("DutSimulator: not connected during ") + op);
    }
}

void DutSimulator::connect()
{
    connected_ = true;
}

void DutSimulator::apply(const DutState& state)
{
    ensure_connected("apply");
    if (rejectNextApply_) {
        rejectNextApply_ = false;
        throw std::runtime_error("DutSimulator: apply confirmation rejected");
    }
    state_ = state;
}

DutState DutSimulator::readback()
{
    ensure_connected("readback");
    return state_;
}

double DutSimulator::temperature_c()
{
    ensure_connected("temperature_c");
    return temperatureC_;
}

void DutSimulator::set_safe_state() noexcept
{
    // Безопасное состояние: channel=1, att_code=0, phase_code=0
    // (нулевые коды аттенюатора и фазы — минимальное влияние на тракт).
    state_.channel = 1;
    state_.att_code = 0;
    state_.phase_code = 0;
}

void DutSimulator::reject_next_apply()
{
    rejectNextApply_ = true;
}

void DutSimulator::simulate_disconnect()
{
    connected_ = false;
}
