#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace afar::cal {

struct NormalizeResult {
    std::complex<double> s_tilde{};
    bool valid{false};
};

/// Комплексная нормировка \(\tilde S = S / R\) (т. 4.5, формула 3).
/// Деление на ноль или неконечный R → valid=false, s_tilde = 0 (без NaN в LUT).
NormalizeResult normalize(std::complex<double> s, std::complex<double> r);

/// Поэлементная нормировка; длины должны совпадать, иначе пустой результат.
std::vector<NormalizeResult> normalize_sweep(
    const std::vector<std::complex<double>>& s,
    const std::vector<std::complex<double>>& r);

/// Линейная интерполяция комплексной опоры по времени между (r0,t0) и (r1,t1).
/// При t0==t1 возвращает r0, если он конечен; иначе невалидно.
NormalizeResult interpolate_reference(
    std::complex<double> r0,
    double t0,
    std::complex<double> r1,
    double t1,
    double t);

}  // namespace afar::cal
