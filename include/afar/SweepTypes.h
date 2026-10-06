#pragma once

#include <complex>
#include <cstdint>
#include <vector>

enum class SParameter : std::uint8_t {
    S11,
    S21,
    S12,
    S22,
};

/// Однопортовая OSL/SOLT1 (без THRU). Порт задаётся отдельно (1|2).
enum class OnePortCalibrationStep : std::uint8_t {
    Begin,
    Open,
    Short,
    Load,
    Apply,
};

enum class TwoPortCalibrationStep : std::uint8_t {
    Begin,
    OpenPort1,
    ShortPort1,
    LoadPort1,
    OpenPort2,
    ShortPort2,
    LoadPort2,
    Thru12,
    Apply,
};

struct SweepConfig {
    std::uint64_t f_start_hz{};
    std::uint64_t f_stop_hz{};
    std::uint32_t points{};
    double power_dbm{};
    std::uint32_t ifbw_hz{};
    std::uint16_t averages{};
    SParameter s_parameter{SParameter::S21};
};

struct ComplexSweep {
    std::vector<std::uint64_t> frequency_hz;
    std::vector<std::complex<double>> s11;
    std::vector<std::complex<double>> s21;
    std::vector<std::complex<double>> s12;
    std::vector<std::complex<double>> s22;
    bool overload{};
};
