#pragma once

#include <complex>
#include <cstdint>

namespace afar::cal {

/// Строка прямой LUT (FR-13 / т. 7.4).
struct DirectLutEntry {
    std::uint8_t channel{};
    std::uint64_t freq_hz{};
    std::uint16_t att_code{};
    std::uint8_t phase_code{};
    double s21_re{};
    double s21_im{};
    double mag_db{};
    double phase_unwrapped_deg{};
    double atten_meas_db{};
    double phase_error_deg{};
    double drift_phase_deg{};
    double repeatability_db{};
    double repeatability_deg{};
    bool valid{false};
};

struct DirectLutBuildInput {
    std::uint8_t channel{};
    std::uint64_t freq_hz{};
    std::uint16_t att_code{};
    std::uint8_t phase_code{};
    std::complex<double> s_tilde{};
    double phase_unwrapped_deg{};
    double nominal_phase_deg{};
    double drift_phase_deg{};
    double repeatability_db{};
    double repeatability_deg{};
    bool sample_valid{true};
};

/// Собирает поля прямой LUT из нормированного комплекса и оценок QC.
DirectLutEntry build_direct_lut_entry(const DirectLutBuildInput& in);

}  // namespace afar::cal
