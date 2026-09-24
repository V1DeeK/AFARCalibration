#pragma once

#include <complex>
#include <cstdint>
#include <vector>

enum class SParameter : std::uint8_t { S11, S21, S12, S22 };

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

[[nodiscard]] constexpr const char* scpi_name(SParameter parameter) noexcept
{
    switch (parameter) {
    case SParameter::S11:
        return "S11";
    case SParameter::S21:
        return "S21";
    case SParameter::S12:
        return "S12";
    case SParameter::S22:
        return "S22";
    }
    return "S21";
}

struct SweepConfig {
    std::uint64_t f_start_hz{};
    std::uint64_t f_stop_hz{};
    std::uint32_t points{};
    double power_dbm{};
    std::uint32_t ifbw_hz{};
    std::uint16_t averages{};
    SParameter parameter{SParameter::S21};
};

struct ComplexSweep {
    std::vector<std::uint64_t> frequency_hz;
    std::vector<std::complex<double>> s21;
    bool overload{};
};
