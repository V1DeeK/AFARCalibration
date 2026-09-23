#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "DirectLut.h"
#include "InverseLut.h"
#include "Normalize.h"
#include "PhaseMath.h"

#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using afar::cal::attenuation_db;
using afar::cal::build_direct_lut_entry;
using afar::cal::DirectLutBuildInput;
using afar::cal::inverse_cost;
using afar::cal::InverseLutCandidate;
using afar::cal::InverseLutWeights;
using afar::cal::interpolate_reference;
using afar::cal::magnitude_db;
using afar::cal::normalize;
using afar::cal::select_inverse_codes;
using afar::cal::unwrap_degrees;
using afar::cal::unwrap_phase_deg;
using afar::cal::wrap180;

namespace {

constexpr double kTol = 1e-10;

}  // namespace

TEST_CASE("normalize: complex S/R and invalid divisor", "[lut_math][AT-09]")
{
    const std::complex<double> s{3.0, -4.0};
    const std::complex<double> r{1.0, 1.0};
    const auto ok = normalize(s, r);
    REQUIRE(ok.valid);
    const std::complex<double> expected = s / r;
    REQUIRE_THAT(ok.s_tilde.real(), WithinAbs(expected.real(), kTol));
    REQUIRE_THAT(ok.s_tilde.imag(), WithinAbs(expected.imag(), kTol));

    const auto zero_r = normalize(s, {0.0, 0.0});
    REQUIRE_FALSE(zero_r.valid);
    REQUIRE_THAT(zero_r.s_tilde.real(), WithinAbs(0.0, kTol));
    REQUIRE_THAT(zero_r.s_tilde.imag(), WithinAbs(0.0, kTol));

    const auto nan_r = normalize(s, {std::numeric_limits<double>::quiet_NaN(), 0.0});
    REQUIRE_FALSE(nan_r.valid);
}

TEST_CASE("normalize: reference time interpolation", "[lut_math][AT-09]")
{
    const auto mid = interpolate_reference({0.0, 0.0}, 0.0, {2.0, 4.0}, 2.0, 1.0);
    REQUIRE(mid.valid);
    REQUIRE_THAT(mid.s_tilde.real(), WithinAbs(1.0, kTol));
    REQUIRE_THAT(mid.s_tilde.imag(), WithinAbs(2.0, kTol));
}

TEST_CASE("attenuation and magnitude dB", "[lut_math][AT-09]")
{
    const std::complex<double> z{0.5, 0.0};
    REQUIRE_THAT(attenuation_db(z), WithinAbs(6.020599913279624, kTol));
    REQUIRE_THAT(magnitude_db(z), WithinAbs(-6.020599913279624, kTol));
    REQUIRE_THAT(attenuation_db(z) + magnitude_db(z), WithinAbs(0.0, kTol));
}

TEST_CASE("wrap180 matches formula (6)", "[lut_math][AT-09]")
{
    REQUIRE_THAT(wrap180(0.0), WithinAbs(0.0, kTol));
    REQUIRE_THAT(wrap180(179.0), WithinAbs(179.0, kTol));
    REQUIRE_THAT(wrap180(180.0), WithinAbs(-180.0, kTol));
    REQUIRE_THAT(wrap180(181.0), WithinAbs(-179.0, kTol));
    REQUIRE_THAT(wrap180(-180.0), WithinAbs(-180.0, kTol));
    REQUIRE_THAT(wrap180(540.0), WithinAbs(-180.0, kTol));
    REQUIRE_THAT(wrap180(-540.0), WithinAbs(-180.0, kTol));
    REQUIRE_THAT(wrap180(270.0), WithinAbs(-90.0, kTol));
}

TEST_CASE("unwrap phase along axis", "[lut_math][AT-09]")
{
    // Phase steps of +170° wrapped would jump; unwrapped should accumulate.
    const std::vector<double> wrapped{0.0, 170.0, -20.0, 150.0};
    const auto unwrapped = unwrap_degrees(wrapped);
    REQUIRE(unwrapped.size() == 4);
    REQUIRE_THAT(unwrapped[0], WithinAbs(0.0, kTol));
    REQUIRE_THAT(unwrapped[1], WithinAbs(170.0, kTol));
    REQUIRE_THAT(unwrapped[2], WithinAbs(340.0, kTol));
    REQUIRE_THAT(unwrapped[3], WithinAbs(510.0, kTol));

    using namespace std::complex_literals;
    const double rad = std::numbers::pi_v<double> / 2.0;
    const std::vector<std::complex<double>> zs{
        {1.0, 0.0},
        {std::cos(rad), std::sin(rad)},
        {std::cos(2.0 * rad), std::sin(2.0 * rad)},
        {std::cos(3.0 * rad), std::sin(3.0 * rad)},
    };
    const auto deg = unwrap_phase_deg(zs);
    REQUIRE(deg.size() == 4);
    REQUIRE_THAT(deg[0], WithinAbs(0.0, kTol));
    REQUIRE_THAT(deg[1], WithinAbs(90.0, kTol));
    REQUIRE_THAT(deg[2], WithinAbs(180.0, kTol));
    REQUIRE_THAT(deg[3], WithinAbs(270.0, kTol));
}

TEST_CASE("direct LUT fields from normalized sample", "[lut_math][AT-09]")
{
    DirectLutBuildInput in;
    in.channel = 1;
    in.freq_hz = 5'000'000'000ULL;
    in.att_code = 3;
    in.phase_code = 4;
    in.s_tilde = {0.5, 0.0};
    in.phase_unwrapped_deg = 22.5;
    in.nominal_phase_deg = 22.5;
    in.drift_phase_deg = 0.1;
    in.repeatability_db = 0.01;
    in.repeatability_deg = 0.2;
    in.sample_valid = true;

    const auto e = build_direct_lut_entry(in);
    REQUIRE(e.valid);
    REQUIRE(e.channel == 1);
    REQUIRE(e.att_code == 3);
    REQUIRE(e.phase_code == 4);
    REQUIRE_THAT(e.s21_re, WithinAbs(0.5, kTol));
    REQUIRE_THAT(e.atten_meas_db, WithinAbs(attenuation_db(in.s_tilde), kTol));
    REQUIRE_THAT(e.mag_db, WithinAbs(magnitude_db(in.s_tilde), kTol));
    REQUIRE_THAT(e.phase_error_deg, WithinAbs(0.0, kTol));
}

TEST_CASE("inverse LUT selects min J and prefers valid", "[lut_math][AT-09]")
{
    const InverseLutWeights w{1.0, 1.0, 1.0e12};
    const double a_target = 6.0;
    const double phi_target = 45.0;

    const std::vector<InverseLutCandidate> cands{
        {10, 1, 10.0, 45.0, true},   // farther in A
        {11, 2, 6.1, 46.0, true},    // closer
        {12, 3, 6.0, 45.0, false},   // exact but invalid — must not win
    };

    const auto j_exact_invalid = inverse_cost(6.0, 45.0, a_target, phi_target, false, w);
    const auto j_near_valid = inverse_cost(6.1, 46.0, a_target, phi_target, true, w);
    REQUIRE(j_exact_invalid > j_near_valid);

    const auto sel = select_inverse_codes(a_target, phi_target, cands, w);
    REQUIRE(sel.found);
    REQUIRE(sel.valid);
    REQUIRE(sel.selected_att_code == 11);
    REQUIRE(sel.selected_phase_code == 2);
    REQUIRE_THAT(sel.atten_residual_db, WithinAbs(0.1, kTol));
    REQUIRE_THAT(sel.phase_residual_deg, WithinAbs(1.0, kTol));

    // Synthetic exact match to double tolerance.
    const std::vector<InverseLutCandidate> exact{
        {0, 0, 3.0, -90.0, true},
        {7, 9, a_target, phi_target, true},
    };
    const auto hit = select_inverse_codes(a_target, phi_target, exact, w);
    REQUIRE(hit.found);
    REQUIRE(hit.selected_att_code == 7);
    REQUIRE(hit.selected_phase_code == 9);
    REQUIRE_THAT(hit.cost_j, WithinAbs(0.0, kTol));
    REQUIRE_THAT(hit.atten_residual_db, WithinAbs(0.0, kTol));
    REQUIRE_THAT(hit.phase_residual_deg, WithinAbs(0.0, kTol));
}
