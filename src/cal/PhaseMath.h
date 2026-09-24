#pragma once

#include <complex>
#include <cstdint>
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

struct FilterMetrics {
    bool valid{};
    bool has_3db_band{};
    std::uint64_t peak_frequency_hz{};
    double peak_db{};
    double insertion_loss_db{};
    double lower_3db_hz{};
    double upper_3db_hz{};
    double center_hz{};
    double bandwidth_3db_hz{};
    double max_stopband_rejection_db{};
};

/// Минимальные метрики полосового фильтра по комплексной S21.
FilterMetrics analyze_filter(const std::vector<std::uint64_t>& frequency_hz,
                             const std::vector<std::complex<double>>& s21);

}  // namespace afar::cal
