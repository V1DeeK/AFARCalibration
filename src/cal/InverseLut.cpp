#include "InverseLut.h"

#include "PhaseMath.h"

#include <cmath>
#include <limits>

namespace afar::cal {

double inverse_cost(
    double a_meas_db,
    double phi_meas_deg,
    double a_target_db,
    double phi_target_deg,
    bool valid,
    const InverseLutWeights& weights)
{
    const double da = a_meas_db - a_target_db;
    const double dphi = wrap180(phi_meas_deg - phi_target_deg);
    const double penalty = valid ? 0.0 : weights.p_invalid;
    return weights.w_a * da * da + weights.w_phi * dphi * dphi + penalty;
}

InverseLutResult select_inverse_codes(
    double a_target_db,
    double phi_target_deg,
    std::span<const InverseLutCandidate> candidates,
    const InverseLutWeights& weights)
{
    InverseLutResult best;
    if (candidates.empty()) {
        return best;
    }

    bool any_valid = false;
    for (const auto& c : candidates) {
        if (c.valid) {
            any_valid = true;
            break;
        }
    }

    double best_j = std::numeric_limits<double>::infinity();
    const InverseLutCandidate* chosen = nullptr;

    for (const auto& c : candidates) {
        if (any_valid && !c.valid) {
            continue;
        }
        if (!std::isfinite(c.a_meas_db) || !std::isfinite(c.phi_meas_deg)) {
            continue;
        }
        const double j = inverse_cost(
            c.a_meas_db,
            c.phi_meas_deg,
            a_target_db,
            phi_target_deg,
            c.valid,
            weights);
        if (!std::isfinite(j)) {
            continue;
        }
        if (chosen == nullptr || j < best_j) {
            best_j = j;
            chosen = &c;
        }
    }

    if (chosen == nullptr) {
        return best;
    }

    best.found = true;
    best.selected_att_code = chosen->att_code;
    best.selected_phase_code = chosen->phase_code;
    best.measured_atten_db = chosen->a_meas_db;
    best.measured_phase_deg = chosen->phi_meas_deg;
    best.atten_residual_db = chosen->a_meas_db - a_target_db;
    best.phase_residual_deg = wrap180(chosen->phi_meas_deg - phi_target_deg);
    best.cost_j = best_j;
    best.valid = chosen->valid;
    return best;
}

}  // namespace afar::cal
