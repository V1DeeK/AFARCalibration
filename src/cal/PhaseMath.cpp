#include "PhaseMath.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace afar::cal {

double magnitude_db(std::complex<double> s_tilde)
{
    const double mag = std::abs(s_tilde);
    if (!(mag > 0.0) || !std::isfinite(mag)) {
        return -std::numeric_limits<double>::infinity();
    }
    return 20.0 * std::log10(mag);
}

double attenuation_db(std::complex<double> s_tilde)
{
    const double mag = std::abs(s_tilde);
    if (!(mag > 0.0) || !std::isfinite(mag)) {
        return std::numeric_limits<double>::infinity();
    }
    return -20.0 * std::log10(mag);
}

double vswr_from_reflection_db(double reflection_db)
{
    const double gamma = std::pow(10.0, reflection_db / 20.0);
    if (!std::isfinite(gamma) || gamma >= 1.0) {
        return std::numeric_limits<double>::infinity();
    }
    return (1.0 + gamma) / (1.0 - gamma);
}

double arg_deg(std::complex<double> s_tilde)
{
    return std::atan2(s_tilde.imag(), s_tilde.real()) * (180.0 / std::numbers::pi_v<double>);
}

double wrap180(double x_deg)
{
    if (!std::isfinite(x_deg)) {
        return x_deg;
    }
    double y = std::fmod(x_deg + 180.0, 360.0);
    if (y < 0.0) {
        y += 360.0;
    }
    return y - 180.0;
}

std::vector<double> unwrap_degrees(const std::vector<double>& wrapped_deg)
{
    std::vector<double> out;
    if (wrapped_deg.empty()) {
        return out;
    }
    out.resize(wrapped_deg.size());
    out[0] = wrapped_deg[0];
    for (std::size_t i = 1; i < wrapped_deg.size(); ++i) {
        const double step = wrap180(wrapped_deg[i] - wrapped_deg[i - 1]);
        out[i] = out[i - 1] + step;
    }
    return out;
}

namespace {

bool is_reference_measurement(std::complex<double> z)
{
    if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) {
        return false;
    }
    const double mag2 = z.real() * z.real() + z.imag() * z.imag();
    return mag2 > 0.0 && std::isfinite(mag2);
}

}  // namespace

std::optional<double> reference_drift_phase_deg(
    std::complex<double> r_prev,
    std::complex<double> r_curr)
{
    if (!is_reference_measurement(r_prev) || !is_reference_measurement(r_curr)) {
        return std::nullopt;
    }
    return wrap180(arg_deg(r_curr) - arg_deg(r_prev));
}

std::optional<RepeatabilityEstimate> repeatability_from_attempts(
    const std::vector<std::complex<double>>& attempts)
{
    std::vector<std::complex<double>> measured;
    measured.reserve(attempts.size());
    for (const auto& z : attempts) {
        if (is_reference_measurement(z)) {
            measured.push_back(z);
        }
    }
    if (measured.size() < 2) {
        return std::nullopt;
    }

    const double mag0 = magnitude_db(measured.front());
    double db_span = 0.0;
    double deg_span = 0.0;
    const double arg0 = arg_deg(measured.front());
    for (std::size_t i = 1; i < measured.size(); ++i) {
        const double db = std::fabs(magnitude_db(measured[i]) - mag0);
        const double deg = std::fabs(wrap180(arg_deg(measured[i]) - arg0));
        if (db > db_span) {
            db_span = db;
        }
        if (deg > deg_span) {
            deg_span = deg;
        }
    }
    if (!std::isfinite(db_span) || !std::isfinite(deg_span)) {
        return std::nullopt;
    }
    return RepeatabilityEstimate{db_span, deg_span};
}

std::vector<double> unwrap_phase_deg(const std::vector<std::complex<double>>& s_tilde)
{
    std::vector<double> wrapped;
    wrapped.reserve(s_tilde.size());
    for (const auto& z : s_tilde) {
        wrapped.push_back(arg_deg(z));
    }
    return unwrap_degrees(wrapped);
}

}  // namespace afar::cal
