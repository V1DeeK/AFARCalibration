#pragma once

#include "SweepTypes.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace afar::c2220 {

inline constexpr std::uint64_t kFrequencyMinHz = 100'000ULL;
inline constexpr std::uint64_t kFrequencyMaxHz = 20'000'000'000ULL;
inline constexpr std::uint32_t kPointsMin = 2;
inline constexpr std::uint32_t kPointsMax = 500'001;
inline constexpr std::uint32_t kIfbwMinHz = 1;
inline constexpr std::uint32_t kIfbwMaxHz = 1'000'000;
inline constexpr double kPowerMinDbm = -60.0;
inline constexpr double kPowerMaxDbm = 10.0;
inline constexpr std::uint16_t kAveragesMin = 1;
inline constexpr std::uint16_t kAveragesMax = 999;

inline bool validateSweep(const SweepConfig& config, std::string& diagnostics)
{
    diagnostics.clear();
    if (config.f_start_hz < kFrequencyMinHz || config.f_start_hz > kFrequencyMaxHz) {
        diagnostics = "f_start_hz must be in 100000..20000000000";
    } else if (config.f_stop_hz < kFrequencyMinHz
               || config.f_stop_hz > kFrequencyMaxHz) {
        diagnostics = "f_stop_hz must be in 100000..20000000000";
    } else if (config.f_start_hz >= config.f_stop_hz) {
        diagnostics = "f_start_hz must be strictly less than f_stop_hz";
    } else if (config.points < kPointsMin || config.points > kPointsMax) {
        diagnostics = "points must be in 2..500001";
    } else if (config.ifbw_hz < kIfbwMinHz || config.ifbw_hz > kIfbwMaxHz) {
        diagnostics = "ifbw_hz must be in 1..1000000";
    } else if (!std::isfinite(config.power_dbm)
               || config.power_dbm < kPowerMinDbm || config.power_dbm > kPowerMaxDbm) {
        diagnostics = "power_dbm must be finite and in -60..10";
    } else if (config.averages < kAveragesMin || config.averages > kAveragesMax) {
        diagnostics = "averages must be in 1..999";
    }
    return diagnostics.empty();
}

}  // namespace afar::c2220
