#pragma once

#include <complex>
#include <vector>

namespace afar::cal {

/// \(A = -20\log_{10}|\tilde S|\) (формула 4). При |S|=0 или неконечном — +Inf.
double attenuation_db(std::complex<double> s_tilde);

/// \(20\log_{10}|\tilde S|\) — модуль в дБ.
double magnitude_db(std::complex<double> s_tilde);

/// \(\arg(\tilde S)\) в градусах, без развёртки, диапазон (−180, 180].
double arg_deg(std::complex<double> s_tilde);

/// \(\mathrm{wrap180}(x) = ((x + 180) \bmod 360) - 180\) (формула 6), диапазон [−180, 180).
double wrap180(double x_deg);

/// Развёртка фазы вдоль оси (формула 5): unwrap(arg)·180/π в градусах.
std::vector<double> unwrap_phase_deg(const std::vector<std::complex<double>>& s_tilde);

/// Развёртка уже угловых (градусных) отсчётов.
std::vector<double> unwrap_degrees(const std::vector<double>& wrapped_deg);

}  // namespace afar::cal
