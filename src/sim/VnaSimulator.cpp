#include "VnaSimulator.h"

#include <cmath>
#include <complex>
#include <limits>
#include <utility>

namespace {

constexpr const char* kDefaultIdn = "PLANAR,C2220,SIM0001,1.0";
constexpr const char* kWrongModelIdn = "OTHERVENDOR,X9999,0000,0.0";

std::complex<double> synthetic_s21(std::uint64_t frequency_hz)
{
    // Известная функция только от f: |S21|=0.8, arg = 2*pi*(f/1e9).
    constexpr double kMag = 0.8;
    const double phase_rad =
        2.0 * 3.14159265358979323846 * (static_cast<double>(frequency_hz) / 1.0e9);
    return std::polar(kMag, phase_rad);
}

}  // namespace

void VnaSimulator::throw_if_failure_on_io(const char* op)
{
    switch (failureMode_) {
    case FailureMode::Timeout:
        throw std::runtime_error(std::string("VnaSimulator: timeout during ") + op);
    case FailureMode::Disconnect:
        connected_ = false;
        throw std::runtime_error(std::string("VnaSimulator: disconnected during ") + op);
    default:
        break;
    }
}

void VnaSimulator::connect()
{
    throw_if_failure_on_io("connect");
    connected_ = true;
}

std::string VnaSimulator::identify()
{
    throw_if_failure_on_io("identify");
    if (!connected_) {
        throw std::runtime_error("VnaSimulator: identify without connect");
    }
    if (failureMode_ == FailureMode::WrongModel) {
        return kWrongModelIdn;
    }
    return identifyString_;
}

void VnaSimulator::configure(const SweepConfig& config)
{
    throw_if_failure_on_io("configure");
    if (!connected_) {
        throw std::runtime_error("VnaSimulator: configure without connect");
    }
    if (config.points == 0) {
        throw std::invalid_argument("VnaSimulator: points must be > 0");
    }
    if (config.f_stop_hz < config.f_start_hz) {
        throw std::invalid_argument("VnaSimulator: f_stop_hz < f_start_hz");
    }
    config_ = config;
    configured_ = true;
}

ComplexSweep VnaSimulator::measure_s21()
{
    throw_if_failure_on_io("measure_s21");
    if (!connected_) {
        throw std::runtime_error("VnaSimulator: measure_s21 without connect");
    }
    if (!configured_) {
        throw std::runtime_error("VnaSimulator: measure_s21 without configure");
    }

    ComplexSweep sweep;
    sweep.frequency_hz.resize(config_.points);
    sweep.s21.resize(config_.points);
    sweep.overload = (failureMode_ == FailureMode::Overload);

    const auto n = config_.points;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t f = config_.f_start_hz;
        if (n > 1) {
            const auto span = config_.f_stop_hz - config_.f_start_hz;
            f = config_.f_start_hz
                + (span * static_cast<std::uint64_t>(i))
                      / static_cast<std::uint64_t>(n - 1);
        }
        sweep.frequency_hz[i] = f;
        if (failureMode_ == FailureMode::Nan) {
            sweep.s21[i] = {
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
        } else {
            sweep.s21[i] = synthetic_s21(f);
        }
    }
    return sweep;
}

void VnaSimulator::calibrate_two_port(TwoPortCalibrationStep step)
{
    throw_if_failure_on_io("calibrate_two_port");
    if (!connected_) {
        throw std::runtime_error("VnaSimulator: calibrate_two_port without connect");
    }
    calibrationStep_ = step;
}

std::vector<std::string> VnaSimulator::drain_errors()
{
    // Режим drain_errors: отдаём накопленную очередь SYST:ERR?-подобных строк.
    auto out = std::move(errorQueue_);
    errorQueue_.clear();
    return out;
}

void VnaSimulator::abort() noexcept
{
    // Безопасный no-op: не бросает, не трогает железо.
}

void VnaSimulator::set_failure_mode(FailureMode mode)
{
    failureMode_ = mode;
    if (mode == FailureMode::None && identifyString_.empty()) {
        identifyString_ = kDefaultIdn;
    }
}

void VnaSimulator::set_identify_string(std::string idn)
{
    identifyString_ = std::move(idn);
}

void VnaSimulator::push_instrument_error(std::string message)
{
    errorQueue_.push_back(std::move(message));
}
