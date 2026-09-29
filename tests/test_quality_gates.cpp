#include <catch2/catch_test_macros.hpp>

#include "InverseLut.h"
#include "QualityGates.h"

#include <cmath>
#include <limits>
#include <vector>

using afar::cal::InverseLutCandidate;
using afar::cal::InverseLutWeights;
using afar::cal::select_inverse_codes;
using afar::qc::evaluate_fail_reason;
using afar::qc::evaluate_valid;
using afar::qc::QualityFailReason;
using afar::qc::QualityInputs;
using afar::qc::QualityThresholds;

TEST_CASE("FR-17 default thresholds are parameters", "[quality_gates][AT-10]")
{
    const QualityThresholds t{};
    REQUIRE(t.max_drift_phase_deg == 1.0);
    REQUIRE(t.max_phase_residual_deg == 2.8125);
    REQUIRE(t.max_repeatability_db == 0.10);
    REQUIRE(t.max_repeatability_deg == 1.0);
}

TEST_CASE("NaN / Inf → valid=false", "[quality_gates][AT-10]")
{
    QualityInputs in;
    in.has_nan_or_inf = true;
    REQUIRE_FALSE(evaluate_valid(in));
    REQUIRE(evaluate_fail_reason(in) == QualityFailReason::NanOrInf);
}

TEST_CASE("overload → valid=false", "[quality_gates][AT-10]")
{
    QualityInputs in;
    in.overload = true;
    REQUIRE_FALSE(evaluate_valid(in));
    REQUIRE(evaluate_fail_reason(in) == QualityFailReason::Overload);
}

TEST_CASE("drift above FR-17 → valid=false", "[quality_gates][AT-10]")
{
    QualityThresholds t;  // drift 1.0°
    QualityInputs in;
    in.drift_phase_deg = 1.0;
    REQUIRE(evaluate_valid(in, t));

    in.drift_phase_deg = 1.0000001;
    REQUIRE_FALSE(evaluate_valid(in, t));
    REQUIRE(evaluate_fail_reason(in, t) == QualityFailReason::DriftExceeded);

    in.drift_phase_deg = -1.5;
    REQUIRE_FALSE(evaluate_valid(in, t));
}

TEST_CASE("repeatability limits", "[quality_gates][AT-10]")
{
    QualityThresholds t;
    QualityInputs in;
    in.repeatability_db = 0.10;
    in.repeatability_deg = 1.0;
    REQUIRE(evaluate_valid(in, t));

    in.repeatability_db = 0.11;
    REQUIRE_FALSE(evaluate_valid(in, t));
    REQUIRE(evaluate_fail_reason(in, t) == QualityFailReason::RepeatabilityExceeded);

    in.repeatability_db = 0.0;
    in.repeatability_deg = 1.1;
    REQUIRE_FALSE(evaluate_valid(in, t));
}

TEST_CASE("inverse LUT skips invalid when valid exists", "[quality_gates][AT-10]")
{
    QualityThresholds thresholds;
    QualityInputs bad;
    bad.overload = true;
    REQUIRE_FALSE(evaluate_valid(bad, thresholds));

    QualityInputs good;
    good.drift_phase_deg = 0.2;
    REQUIRE(evaluate_valid(good, thresholds));

    // Candidates: invalid is a perfect match, valid is slightly off.
    const std::vector<InverseLutCandidate> cands{
        {1, 1, 5.0, 10.0, false},  // exact target, invalid
        {2, 2, 5.2, 11.0, true},   // near, valid
    };
    const InverseLutWeights w{};
    const auto sel = select_inverse_codes(5.0, 10.0, cands, w);
    REQUIRE(sel.found);
    REQUIRE(sel.valid);
    REQUIRE(sel.selected_att_code == 2);
    REQUIRE(sel.selected_phase_code == 2);

    // Only invalid left — may be selected, but marked valid=false.
    const std::vector<InverseLutCandidate> only_bad{
        {9, 9, 5.0, 10.0, false},
    };
    const auto forced = select_inverse_codes(5.0, 10.0, only_bad, w);
    REQUIRE(forced.found);
    REQUIRE_FALSE(forced.valid);
    REQUIRE(forced.selected_att_code == 9);
}

TEST_CASE("other FR-16 gates", "[quality_gates][AT-10]")
{
    REQUIRE_FALSE(evaluate_valid(QualityInputs{.code_confirmed = false}));
    REQUIRE_FALSE(evaluate_valid(QualityInputs{.vna_timeout_or_error = true}));
    REQUIRE_FALSE(evaluate_valid(QualityInputs{.length_mismatch = true}));
}

TEST_CASE("FR-17 phase residual gate for inverse LUT", "[quality_gates][AT-10]")
{
    using afar::qc::phase_residual_within_limit;
    QualityThresholds t;  // 2.8125°
    REQUIRE(phase_residual_within_limit(2.8125, t));
    REQUIRE_FALSE(phase_residual_within_limit(2.8125001, t));
    REQUIRE_FALSE(phase_residual_within_limit(-3.0, t));
}
