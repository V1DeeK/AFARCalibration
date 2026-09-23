#pragma once

#include <cstdint>
#include <span>

namespace afar::cal {

/// Веса и штраф формулы (7); задаются конфигурацией.
struct InverseLutWeights {
    double w_a{1.0};
    double w_phi{1.0};
    double p_invalid{1.0e12};
};

struct InverseLutCandidate {
    std::uint16_t att_code{};
    std::uint8_t phase_code{};
    double a_meas_db{};
    double phi_meas_deg{};
    bool valid{false};
};

struct InverseLutResult {
    std::uint16_t selected_att_code{};
    std::uint8_t selected_phase_code{};
    double measured_atten_db{};
    double measured_phase_deg{};
    double atten_residual_db{};
    double phase_residual_deg{};
    double cost_j{};
    bool valid{false};
    bool found{false};
};

/// Функционал \(J = w_A(A_{meas}-A_{target})^2 + w_\phi\mathrm{wrap180}^2(\phi_{meas}-\phi_{target}) + P_{invalid}\).
double inverse_cost(
    double a_meas_db,
    double phi_meas_deg,
    double a_target_db,
    double phi_target_deg,
    bool valid,
    const InverseLutWeights& weights);

/// Выбор кодов по минимуму J. `valid=false` не выбирается, если есть `valid=true` (FR-15).
InverseLutResult select_inverse_codes(
    double a_target_db,
    double phi_target_deg,
    std::span<const InverseLutCandidate> candidates,
    const InverseLutWeights& weights = {});

}  // namespace afar::cal
