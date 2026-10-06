#pragma once

namespace afar::qc {

/// Стартовые пороги FR-17 — параметры ПО, не метрология изделия.
struct QualityThresholds {
    double max_drift_phase_deg{1.0};
    double max_phase_residual_deg{2.8125};
    double max_repeatability_db{0.10};
    double max_repeatability_deg{1.0};
};

/// Входы FR-16 для одной точки / состояния.
struct QualityInputs {
    bool code_confirmed{true};
    bool vna_timeout_or_error{false};
    bool length_mismatch{false};
    bool has_nan_or_inf{false};
    bool overload{false};
    double drift_phase_deg{0.0};
    double repeatability_db{0.0};
    double repeatability_deg{0.0};
};

enum class QualityFailReason {
    None,
    NoCodeConfirm,
    VnaTimeoutOrError,
    LengthMismatch,
    NanOrInf,
    Overload,
    DriftExceeded,
    RepeatabilityExceeded,
};

/// Возвращает true, если точка пригодна (valid=true).
bool evaluate_valid(
    const QualityInputs& in,
    const QualityThresholds& thresholds = {});

QualityFailReason evaluate_fail_reason(
    const QualityInputs& in,
    const QualityThresholds& thresholds = {});

/// Остаток фазы обратной LUT выше порога FR-17 → непригодно для отчёта точки.
bool phase_residual_within_limit(
    double phase_residual_deg,
    const QualityThresholds& thresholds = {});

}  // namespace afar::qc
