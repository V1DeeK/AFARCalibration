#include "Normalize.h"

#include <cmath>

namespace afar::cal {
namespace {

bool is_finite_complex(std::complex<double> z)
{
    return std::isfinite(z.real()) && std::isfinite(z.imag());
}

}  // namespace

NormalizeResult normalize(std::complex<double> s, std::complex<double> r)
{
    NormalizeResult out;
    if (!is_finite_complex(s) || !is_finite_complex(r)) {
        return out;
    }
    const double mag2 = r.real() * r.real() + r.imag() * r.imag();
    if (!(mag2 > 0.0) || !std::isfinite(mag2)) {
        return out;
    }
    out.s_tilde = s / r;
    if (!is_finite_complex(out.s_tilde)) {
        out.s_tilde = {};
        return out;
    }
    out.valid = true;
    return out;
}

std::vector<NormalizeResult> normalize_sweep(
    const std::vector<std::complex<double>>& s,
    const std::vector<std::complex<double>>& r)
{
    std::vector<NormalizeResult> out;
    if (s.size() != r.size()) {
        return out;
    }
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out.push_back(normalize(s[i], r[i]));
    }
    return out;
}

NormalizeResult interpolate_reference(
    std::complex<double> r0,
    double t0,
    std::complex<double> r1,
    double t1,
    double t)
{
    NormalizeResult out;
    if (!std::isfinite(t0) || !std::isfinite(t1) || !std::isfinite(t)) {
        return out;
    }
    if (!is_finite_complex(r0) || !is_finite_complex(r1)) {
        return out;
    }
    if (t0 == t1) {
        out.s_tilde = r0;
        out.valid = true;
        return out;
    }
    const double alpha = (t - t0) / (t1 - t0);
    if (!std::isfinite(alpha)) {
        return out;
    }
    out.s_tilde = r0 + alpha * (r1 - r0);
    if (!is_finite_complex(out.s_tilde)) {
        out.s_tilde = {};
        return out;
    }
    out.valid = true;
    return out;
}

}  // namespace afar::cal
