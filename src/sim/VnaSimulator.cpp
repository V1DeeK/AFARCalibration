#include "VnaSimulator.h"

#include "afar/C2220Limits.h"

#include <cmath>
#include <complex>
#include <limits>
#include <utility>

namespace {

constexpr const char* kDefaultIdn = "PLANAR,C2220,SIM0001,1.0";
constexpr const char* kWrongModelIdn = "OTHERVENDOR,X9999,0000,0.0";
constexpr double kPi = 3.14159265358979323846;

std::complex<double> synthetic_s21(std::uint64_t frequency_hz)
{
    // Известная функция только от f: |S21|=0.8, arg = 2*pi*(f/1e9).
    constexpr double kMag = 0.8;
    const double phase_rad =
        2.0 * 3.14159265358979323846 * (static_cast<double>(frequency_hz) / 1.0e9);
    return std::polar(kMag, phase_rad);
}

std::complex<double> synthetic_s11(std::uint64_t frequency_hz)
{
    // Reflection-like: |S11|=0.3, фаза от частоты (отличимо от S21).
    constexpr double kMag = 0.3;
    const double phase_rad =
        3.14159265358979323846 * (static_cast<double>(frequency_hz) / 1.0e9);
    return std::polar(kMag, phase_rad);
}

std::complex<double> synthetic_value(SParameter p, std::uint64_t frequency_hz)
{
    switch (p) {
    case SParameter::S11:
        return synthetic_s11(frequency_hz);
    case SParameter::S21:
        return synthetic_s21(frequency_hz);
    case SParameter::S12:
        return {0.05, -0.02};  // отличимая константа
    case SParameter::S22:
        return {0.25, 0.1};  // отличимая константа
    }
    return synthetic_s21(frequency_hz);
}

std::complex<double> linked_demo_value(SParameter p,
                                       std::uint64_t frequency_hz,
                                       const DutState& state)
{
    const double frequency_ghz = static_cast<double>(frequency_hz) / 1.0e9;
    const double channel = std::max(1u, static_cast<unsigned>(state.channel));
    const double att = static_cast<double>(state.att_code);
    const double phase = static_cast<double>(state.phase_code);
    const double channel_phase_rad = (channel - 1.0) * 0.7 * kPi / 180.0;

    switch (p) {
    case SParameter::S21: {
        const double ripple_db = 0.04 * std::sin(2.0 * kPi * frequency_ghz
                                                + 0.17 * att + 0.11 * channel);
        const double attenuation_db = 0.5 * att + 0.03 * (channel - 1.0) + ripple_db;
        const double magnitude = 0.8 * std::pow(10.0, -attenuation_db / 20.0);
        const double phase_rad = 2.0 * kPi * frequency_ghz
            + phase * kPi / 32.0 + channel_phase_rad
            + 0.4 * std::sin(kPi * frequency_ghz + 0.13 * phase) * kPi / 180.0;
        return std::polar(magnitude, phase_rad);
    }
    case SParameter::S11:
        return std::polar(0.22 + 0.001 * att,
                          kPi * frequency_ghz + channel_phase_rad);
    case SParameter::S12:
        return std::polar(0.045 + 0.0001 * phase,
                          -0.4 + channel_phase_rad);
    case SParameter::S22:
        return std::polar(0.24 + 0.0005 * att,
                          0.35 * kPi * frequency_ghz - channel_phase_rad);
    }
    return synthetic_s21(frequency_hz);
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
    std::string diagnostics;
    if (!afar::c2220::validateSweep(config, diagnostics)) {
        throw std::invalid_argument("VnaSimulator: " + diagnostics);
    }
    config_ = config;
    configured_ = true;
}

SweepConfig VnaSimulator::read_config()
{
    throw_if_failure_on_io("read_config");
    if (!connected_ || !configured_) {
        throw std::runtime_error("VnaSimulator::read_config without configure");
    }
    return config_;
}

ComplexSweep VnaSimulator::measure_trace()
{
    throw_if_failure_on_io("measure_trace");
    if (!connected_) {
        throw std::runtime_error("VnaSimulator: measure_trace without connect");
    }
    if (!configured_) {
        throw std::runtime_error("VnaSimulator: measure_trace without configure");
    }
    if (failedTrace_ == config_.s_parameter) {
        throw std::runtime_error("VnaSimulator: persistent trace failure");
    }
    if (failNextTrace_ == config_.s_parameter) {
        failNextTrace_.reset();
        throw std::runtime_error("VnaSimulator: transient trace failure");
    }

    ComplexSweep sweep;
    sweep.frequency_hz.resize(config_.points);
    sweep.overload = (failureMode_ == FailureMode::Overload);

    std::vector<std::complex<double>>* trace = nullptr;
    switch (config_.s_parameter) {
    case SParameter::S11:
        trace = &sweep.s11;
        break;
    case SParameter::S21:
        trace = &sweep.s21;
        break;
    case SParameter::S12:
        trace = &sweep.s12;
        break;
    case SParameter::S22:
        trace = &sweep.s22;
        break;
    }
    trace->resize(config_.points);

    const auto n = config_.points;
    const std::optional<DutState> dutState = dutStateProvider_
        ? std::optional<DutState>{dutStateProvider_()} : std::nullopt;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t f = config_.f_start_hz;
        if (n > 1) {
            const auto span = config_.f_stop_hz - config_.f_start_hz;
            f = config_.f_start_hz
                + (span * static_cast<std::uint64_t>(i))
                      / static_cast<std::uint64_t>(n - 1);
        }
        f += frequencyOffsetHz_;
        sweep.frequency_hz[i] = f;
        if (failureMode_ == FailureMode::Nan) {
            (*trace)[i] = {
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
        } else {
            (*trace)[i] = dutState
                ? linked_demo_value(config_.s_parameter, f, *dutState)
                : synthetic_value(config_.s_parameter, f);
        }
    }
    return sweep;
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
    if (config_.s_parameter != SParameter::S21) {
        throw std::runtime_error("VnaSimulator: measure_s21 requires s_parameter == S21");
    }
    return measure_trace();
}

void VnaSimulator::calibrate_one_port(OnePortCalibrationStep step, int port)
{
    throw_if_failure_on_io("calibrate_one_port");
    if (!connected_) {
        throw std::runtime_error("VnaSimulator: calibrate_one_port without connect");
    }
    if (port != 1 && port != 2) {
        throw std::runtime_error("VnaSimulator: calibrate_one_port port must be 1 or 2");
    }
    onePortStep_ = step;
    onePort_ = port;
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
    // Как у C2220Vna: сброс флага связи (GAP-LAYER-001 — чужая модель не остаётся «подключённой»).
    connected_ = false;
    configured_ = false;
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
