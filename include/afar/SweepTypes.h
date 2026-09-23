#pragma once

#include <complex>
#include <cstdint>
#include <vector>

struct SweepConfig {
    std::uint64_t f_start_hz{};
    std::uint64_t f_stop_hz{};
    std::uint32_t points{};
    double power_dbm{};
    std::uint32_t ifbw_hz{};
    std::uint16_t averages{};
};

struct ComplexSweep {
    std::vector<std::uint64_t> frequency_hz;
    std::vector<std::complex<double>> s21;
    bool overload{};
};
