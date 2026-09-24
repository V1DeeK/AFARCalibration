#include "PhaseMath.h"

#include <cmath>
#include <limits>
#include <numbers>
#include <algorithm>
#include <iterator>

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

FilterMetrics analyze_filter(const std::vector<std::uint64_t>& frequency_hz,
                             const std::vector<std::complex<double>>& s21)
{
    FilterMetrics result;
    if (frequency_hz.size() != s21.size() || s21.size() < 3) {
        return result;
    }

    std::vector<double> db;
    db.reserve(s21.size());
    for (const auto& sample : s21) {
        db.push_back(magnitude_db(sample));
    }
    const auto peak_it = std::max_element(db.begin(), db.end());
    if (peak_it == db.end() || !std::isfinite(*peak_it)) {
        return result;
    }
    const std::size_t peak = static_cast<std::size_t>(std::distance(db.begin(), peak_it));
    result.valid = true;
    result.peak_frequency_hz = frequency_hz[peak];
    result.peak_db = *peak_it;
    result.insertion_loss_db = -*peak_it;

    const double target = *peak_it - 3.0;
    auto crossing = [&](std::size_t a, std::size_t b) {
        const double ya = db[a];
        const double yb = db[b];
        if (!std::isfinite(ya) || !std::isfinite(yb) || ya == yb) {
            return static_cast<double>(frequency_hz[a]);
        }
        const double t = (target - ya) / (yb - ya);
        return static_cast<double>(frequency_hz[a])
            + t * (static_cast<double>(frequency_hz[b])
                   - static_cast<double>(frequency_hz[a]));
    };

    std::size_t left = peak;
    while (left > 0 && db[left - 1] >= target) {
        --left;
    }
    std::size_t right = peak;
    while (right + 1 < db.size() && db[right + 1] >= target) {
        ++right;
    }
    if (left == 0 || right + 1 >= db.size()) {
        return result;
    }

    result.lower_3db_hz = crossing(left - 1, left);
    result.upper_3db_hz = crossing(right, right + 1);
    result.center_hz = (result.lower_3db_hz + result.upper_3db_hz) / 2.0;
    result.bandwidth_3db_hz = result.upper_3db_hz - result.lower_3db_hz;
    result.has_3db_band = result.bandwidth_3db_hz > 0.0;

    double min_outside = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < db.size(); ++i) {
        if ((i < left || i > right) && std::isfinite(db[i])) {
            min_outside = std::min(min_outside, db[i]);
        }
    }
    if (std::isfinite(min_outside)) {
        result.max_stopband_rejection_db = result.peak_db - min_outside;
    }
    return result;
}

}  // namespace afar::cal
