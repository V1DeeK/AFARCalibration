#include "PhaseMath.h"

#include <cmath>
#include <limits>
#include <numbers>

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
