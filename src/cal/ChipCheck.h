#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

/// Проверка параметров даташитов HMC624A (аттенюатор) и MAPS-010165 (фазовращатель)
/// по измерениям тракта 64×64 и построение таблиц коррекции усиления и фазы.
///
/// Нумерация кодов — как у измерительного драйвера ППКХ4 (measurements.csv):
///   att_code a = 0…63, номинальное ослабление A = 0,5·(63 − a) дБ (a = 63 — 0 дБ);
///   phase_code p = 0…63, номинальный сдвиг 5,625·p°, рост p уменьшает arg(S21).
/// Методика LUT — calibration_16_channels_RU.pdf, эталон — calibration_lut.py
/// (совместный поиск по 4096 состояниям, критерий |H/Ht − 1|²).
namespace afar::cal {

inline constexpr int kChipCodes = 64;
inline constexpr int kChipStates = kChipCodes * kChipCodes;
inline constexpr int kAttenZeroCode = 63;
inline constexpr double kPhaseLsbDeg = 360.0 / 64.0;

/// Индекс состояния: p-мажорный, как в calibration_lut.py (s = 64·p + a).
[[nodiscard]] constexpr int chip_state(int phase_code, int att_code)
{
    return phase_code * kChipCodes + att_code;
}

/// Номинальное ослабление кода аттенюатора, дБ.
[[nodiscard]] constexpr double nominal_atten_db(int att_code)
{
    return 0.5 * static_cast<double>(kAttenZeroCode - att_code);
}

/// Измерения одного канала на одной частоте.
struct ChipGrid {
    int channel = 0;            ///< channel_id из CSV (как записал драйвер)
    double freq_hz = 0;
    std::array<double, kChipStates> gain_db{};
    std::array<double, kChipStates> phase_deg{};
    std::array<bool, kChipStates> present{};

    [[nodiscard]] bool complete() const;
    [[nodiscard]] int count() const;
};

/// HMC624A, Rev. B, табл. 1 (стр. 3). State error — гарантированный предел (Min/Max),
/// step error и relative phase — типовые (Typ).
struct AttenCheck {
    int states_checked = 0;      ///< пар (p, a ≠ 63)
    int state_violations = 0;    ///< |e| > 0,3 + 3 %·A (0,8–6 ГГц) или 0,1 + 5 %·A (ниже)
    double max_excess_db = 0;    ///< наибольшее превышение предела, дБ (0 — нет)
    double rms_state_error_db = 0;
    double max_abs_state_error_db = 0;
    double max_abs_step_error_db = 0;  ///< |шаг − 0,5|, типовое < 0,2
    int step_over_typ = 0;
    double mean_range_db = 0;    ///< фактический диапазон ослабления, средний по p
    double mean_relative_phase_deg = 0;  ///< сдвиг фазы 0 → 31,5 дБ, средний по p; типовой 50° (3–6 ГГц)
    double max_relative_phase_deg = 0;
};

/// MAPS-010165, Rev. V3 (стр. 2): все значения — типовые.
struct PhaseCheck {
    double rms_phase_error_deg = 0;     ///< по 63 ненулевым состояниям, примечание 4; Typ 4°
    double max_abs_phase_error_deg = 0;
    double rms_amp_error_db = 0;        ///< Typ 0,4 дБ (график стр. 4 — ≈0,6 дБ на 5,5 ГГц)
    double amp_peak_to_peak_db = 0;     ///< Attenuation Variation ±0,8 дБ → 1,6 дБ размаха
    std::array<double, 7> bit_shift_deg{};  ///< измеренный сдвиг битов 5,6…180 и суммы всех
    std::array<double, 7> bit_error_deg{};
    std::array<bool, 7> bit_within_typ{};
    bool monotonic = true;              ///< развёрнутый сдвиг растёт с кодом
};

inline constexpr std::array<double, 7> kPhaseBitNominalDeg{5.625, 11.25, 22.5, 45.0, 90.0, 180.0, 354.375};
inline constexpr std::array<int, 7> kPhaseBitCode{1, 2, 4, 8, 16, 32, 63};
inline constexpr std::array<double, 7> kPhaseBitTypDeg{0.8, 1.2, 1.6, 2.4, 3.5, 5.0, 5.2};
inline constexpr double kMapsRmsPhaseTypDeg = 4.0;
inline constexpr double kMapsRmsAmpTypDb = 0.4;
inline constexpr double kMapsAmpVariationTypDb = 0.8;
inline constexpr double kHmcStepErrorTypDb = 0.2;
inline constexpr double kHmcRelativePhaseTypDeg = 50.0;

/// Предел state error HMC624A для номинального ослабления A на частоте f.
[[nodiscard]] double hmc624a_state_limit_db(double atten_db, double freq_hz);

/// Проверка аттенюатора: ослабление считается относительно a = 63 при том же p.
[[nodiscard]] AttenCheck check_attenuator(const ChipGrid& grid);

/// Проверка фазовращателя при нулевом заданном ослаблении (a = 63), опора — p = 0.
[[nodiscard]] PhaseCheck check_phase_shifter(const ChipGrid& grid);

/// Настройки таблицы коррекции (раздел 8 методики).
struct LutOptions {
    int n_phase = 64;            ///< фазовые цели 360·q/n_phase
    double gain_step_db = 0.5;   ///< t_k = k·шаг
    double gain_margin_db = 0;   ///< m в G0 = min_{n,p} G_n(p,63) − m
    bool use_tolerances = false;
    double gain_tol_db = 0.5;
    double phase_tol_deg = 3.0;
};

/// Одна строка LUT: [канал][уровень][фаза] → полные коды.
struct LutEntry {
    std::uint8_t phase_code = 0;
    std::uint8_t att_code = 0;
    double predicted_gain_db = 0;
    double predicted_phase_deg = 0;
    double gain_error_db = 0;
    double phase_error_deg = 0;
    double relative_complex_error = 0;
    std::int8_t status = -1;     ///< −1 допуски не заданы, 0 недостижимо, 1 в допусках
};

/// LUT всех каналов на одной частоте. entries[n][k·n_phase + q].
struct FrequencyLut {
    double freq_hz = 0;
    double reference_phase_deg = 0;   ///< φ_ref = φ первого канала (0, 63)
    double g0_db = 0;
    std::vector<double> target_gains_db;
    std::vector<double> target_phases_deg;
    std::vector<int> channels;
    std::vector<std::vector<LutEntry>> entries;
    std::string error;
};

/// Общий уровень и число уровней по методике: G0 = min_{n,p} G_n(p,63) − m,
/// нижняя граница — max_{n,p} G_n(p,0). Пустой вектор — каналы не перекрываются.
[[nodiscard]] std::vector<double> choose_target_gains(const std::vector<const ChipGrid*>& grids,
                                                      const LutOptions& options, double* g0_db = nullptr);

/// Совместный выбор кодов (p, a) для каждой цели. Все сетки — одна частота, полные.
[[nodiscard]] FrequencyLut build_frequency_lut(const std::vector<const ChipGrid*>& grids, const LutOptions& options);

/// Ближайший индекс фазовой цели для требуемого приращения ψ, градусы (раздел 9).
[[nodiscard]] int lut_phase_index(double offset_deg, int n_phase = 64);

}  // namespace afar::cal
