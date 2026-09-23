#include "QualityGates.h"

#include <cmath>

namespace afar::qc {
namespace {

bool abs_exceeds(double value, double limit)
{
    if (!std::isfinite(value) || !std::isfinite(limit)) {
        return true;
    }
    return std::fabs(value) > limit;
}

}  // namespace

QualityFailReason evaluate_fail_reason(
    const QualityInputs& in,
    const QualityThresholds& thresholds)
{
    if (!in.code_confirmed) {
        return QualityFailReason::NoCodeConfirm;
    }
    if (in.vna_timeout_or_error) {
        return QualityFailReason::VnaTimeoutOrError;
    }
    if (in.length_mismatch) {
        return QualityFailReason::LengthMismatch;
    }
    if (in.has_nan_or_inf) {
        return QualityFailReason::NanOrInf;
    }
    if (in.overload) {
        return QualityFailReason::Overload;
    }
    if (abs_exceeds(in.drift_phase_deg, thresholds.max_drift_phase_deg)) {
        return QualityFailReason::DriftExceeded;
    }
    if (abs_exceeds(in.repeatability_db, thresholds.max_repeatability_db)
        || abs_exceeds(in.repeatability_deg, thresholds.max_repeatability_deg)) {
        return QualityFailReason::RepeatabilityExceeded;
    }
    return QualityFailReason::None;
}

bool evaluate_valid(
    const QualityInputs& in,
    const QualityThresholds& thresholds)
{
    return evaluate_fail_reason(in, thresholds) == QualityFailReason::None;
}

bool phase_residual_within_limit(
    double phase_residual_deg,
    const QualityThresholds& thresholds)
{
    return !abs_exceeds(phase_residual_deg, thresholds.max_phase_residual_deg);
}

}  // namespace afar::qc
