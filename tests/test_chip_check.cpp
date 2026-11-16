// Проверка даташитов HMC624A / MAPS-010165 и совместная LUT усиления и фазы
// (методика calibration_16_channels_RU, эталон calibration_lut.py).
#include "ChipCheck.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace afar::cal;
using Catch::Approx;

namespace {

double wrap(double x)
{
    return x - 360.0 * std::floor((x + 180.0) / 360.0);
}

/// Идеальный тракт: усиление g0 − A(a), фаза phi0 − 5,625·p (рост p уменьшает arg S21).
ChipGrid idealGrid(int channel, double g0, double phi0, double freqHz = 5.5e9)
{
    ChipGrid g;
    g.channel = channel;
    g.freq_hz = freqHz;
    for (int p = 0; p < kChipCodes; ++p) {
        for (int a = 0; a < kChipCodes; ++a) {
            const auto s = static_cast<std::size_t>(chip_state(p, a));
            g.gain_db[s] = g0 - nominal_atten_db(a);
            g.phase_deg[s] = wrap(phi0 - kPhaseLsbDeg * p);
            g.present[s] = true;
        }
    }
    return g;
}

}  // namespace

TEST_CASE("HMC624A: идеальный аттенюатор укладывается в предел", "[chip]")
{
    const ChipGrid g = idealGrid(0, 31.0, 20.0);
    const AttenCheck r = check_attenuator(g);
    CHECK(r.states_checked == 64 * 63);
    CHECK(r.state_violations == 0);
    CHECK(r.max_excess_db == Approx(0.0).margin(1e-9));
    CHECK(r.rms_state_error_db == Approx(0.0).margin(1e-9));
    CHECK(r.max_abs_step_error_db == Approx(0.0).margin(1e-9));
    CHECK(r.mean_range_db == Approx(31.5));
}

TEST_CASE("HMC624A: предел ±(0,3 + 3 %·A), граница входит в допуск", "[chip]")
{
    CHECK(hmc624a_state_limit_db(31.5, 5.5e9) == Approx(1.245));
    CHECK(hmc624a_state_limit_db(10.0, 0.5e9) == Approx(0.6));
    ChipGrid g = idealGrid(0, 31.0, 0.0);
    // Код 0 (31,5 дБ) при p = 5: ровно на границе — не нарушение; чуть дальше — нарушение.
    g.gain_db[static_cast<std::size_t>(chip_state(5, 0))] -= 1.245;
    CHECK(check_attenuator(g).state_violations == 0);
    g.gain_db[static_cast<std::size_t>(chip_state(5, 0))] -= 0.09;
    const AttenCheck r = check_attenuator(g);
    CHECK(r.state_violations == 1);
    CHECK(r.max_excess_db == Approx(0.09).margin(1e-6));
}

TEST_CASE("HMC624A: сдвиг фазы между крайними состояниями", "[chip]")
{
    ChipGrid g = idealGrid(0, 31.0, 170.0);
    for (int p = 0; p < kChipCodes; ++p) {
        // 52° через границу ±180° не должен превратиться в 308°.
        const auto full = static_cast<std::size_t>(chip_state(p, 0));
        g.phase_deg[full] = wrap(g.phase_deg[full] + 52.0);
    }
    const AttenCheck r = check_attenuator(g);
    CHECK(r.mean_relative_phase_deg == Approx(52.0));
    CHECK(r.max_relative_phase_deg == Approx(52.0));
}

TEST_CASE("HMC624A: пропуски не считаются нулём", "[chip]")
{
    ChipGrid g = idealGrid(0, 31.0, 0.0);
    g.present[static_cast<std::size_t>(chip_state(3, 10))] = false;
    g.gain_db[static_cast<std::size_t>(chip_state(3, 10))] = 0.0;
    const AttenCheck r = check_attenuator(g);
    CHECK(r.states_checked == 64 * 63 - 1);
    CHECK(r.state_violations == 0);
    CHECK_FALSE(g.complete());
}

TEST_CASE("MAPS-010165: идеальный фазовращатель", "[chip]")
{
    const PhaseCheck r = check_phase_shifter(idealGrid(0, 31.0, -170.0));
    CHECK(r.rms_phase_error_deg == Approx(0.0).margin(1e-9));
    CHECK(r.rms_amp_error_db == Approx(0.0).margin(1e-9));
    CHECK(r.amp_peak_to_peak_db == Approx(0.0).margin(1e-9));
    CHECK(r.monotonic);
    CHECK(r.bit_shift_deg[3] == Approx(45.0));
    CHECK(r.bit_shift_deg[6] == Approx(354.375));
    for (const bool ok : r.bit_within_typ) {
        CHECK(ok);
    }
}

TEST_CASE("MAPS-010165: бит 45° вне типового, RMS по 63 состояниям", "[chip]")
{
    ChipGrid g = idealGrid(0, 31.0, 0.0);
    // Только состояние p = 8 сдвинуто: измерено 37° вместо 45°.
    const auto s = static_cast<std::size_t>(chip_state(8, kAttenZeroCode));
    g.phase_deg[s] = wrap(0.0 - 37.0);
    g.gain_db[s] += 2.0;
    const PhaseCheck r = check_phase_shifter(g);
    CHECK(r.bit_shift_deg[3] == Approx(37.0));
    CHECK(r.bit_error_deg[3] == Approx(-8.0));
    CHECK_FALSE(r.bit_within_typ[3]);
    CHECK(r.bit_within_typ[2]);
    CHECK(r.rms_phase_error_deg == Approx(8.0 / std::sqrt(63.0)));
    CHECK(r.rms_amp_error_db == Approx(2.0 / std::sqrt(63.0)));
    CHECK(r.amp_peak_to_peak_db == Approx(2.0));
    CHECK(r.max_abs_phase_error_deg == Approx(8.0));
}

TEST_CASE("LUT: идеальные каналы — нулевые ошибки и общая опора", "[chip][lut]")
{
    // Второй канал: +1 дБ и +11,25° (ровно два шага фазы) относительно первого.
    const ChipGrid c0 = idealGrid(0, 30.0, 40.0);
    const ChipGrid c1 = idealGrid(1, 31.0, 51.25);
    const FrequencyLut lut = build_frequency_lut({&c0, &c1}, LutOptions{});
    REQUIRE(lut.error.empty());
    CHECK(lut.g0_db == Approx(30.0));
    CHECK(lut.reference_phase_deg == Approx(40.0));
    // Общий диапазон: сверху 30 (мин. из каналов), снизу −0,5 (макс. из G(p, 0)).
    REQUIRE(lut.target_gains_db.size() == 62);
    CHECK(lut.target_gains_db.back() == Approx(-0.5));
    for (std::size_t n = 0; n < 2; ++n) {
        for (const LutEntry& e : lut.entries[n]) {
            CHECK(e.gain_error_db == Approx(0.0).margin(1e-9));
            CHECK(e.phase_error_deg == Approx(0.0).margin(1e-9));
            CHECK(e.status == -1);
        }
    }
    // Цель (k = 0, q = 0): канал 0 — коды (0, 63); канал 1 — на 1 дБ глубже и на два шага фазы дальше.
    CHECK(int(lut.entries[0][0].phase_code) == 0);
    CHECK(int(lut.entries[0][0].att_code) == 63);
    CHECK(int(lut.entries[1][0].phase_code) == 2);
    CHECK(int(lut.entries[1][0].att_code) == 61);
    // q = 1: целевая фаза растёт на 5,625°, а рост p фазу уменьшает — код фазы 63.
    CHECK(int(lut.entries[0][1].phase_code) == 63);
}

TEST_CASE("LUT: допуски и недостижимая цель", "[chip][lut]")
{
    ChipGrid c0 = idealGrid(0, 30.0, 0.0);
    // Все состояния с кодом фазы 5 уходят на 2° — цель q = 59 достижима только с ошибкой 2°.
    for (int a = 0; a < kChipCodes; ++a) {
        auto& phi = c0.phase_deg[static_cast<std::size_t>(chip_state(5, a))];
        phi = wrap(phi + 2.0);
    }
    LutOptions opt;
    opt.use_tolerances = true;
    opt.gain_tol_db = 0.1;
    opt.phase_tol_deg = 1.0;
    const FrequencyLut lut = build_frequency_lut({&c0}, opt);
    REQUIRE(lut.error.empty());
    const LutEntry& bad = lut.entries[0][59];
    CHECK(bad.status == 0);
    CHECK(std::abs(bad.phase_error_deg) == Approx(2.0));
    CHECK(lut.entries[0][58].status == 1);
}

TEST_CASE("LUT: неполная сетка и пустой общий диапазон отклоняются", "[chip][lut]")
{
    ChipGrid c0 = idealGrid(0, 30.0, 0.0);
    c0.present[100] = false;
    CHECK_FALSE(build_frequency_lut({&c0}, LutOptions{}).error.empty());

    const ChipGrid low = idealGrid(0, 0.0, 0.0);
    const ChipGrid high = idealGrid(1, 40.0, 0.0);
    CHECK_FALSE(build_frequency_lut({&low, &high}, LutOptions{}).error.empty());
    CHECK(build_frequency_lut({}, LutOptions{}).error == "нет каналов");
}

TEST_CASE("LUT: индекс фазовой цели", "[chip][lut]")
{
    CHECK(lut_phase_index(0.0) == 0);
    CHECK(lut_phase_index(5.625) == 1);
    CHECK(lut_phase_index(-5.625) == 63);
    CHECK(lut_phase_index(2.8125) == 1);  // половина шага — к большему индексу
    CHECK(lut_phase_index(359.0) == 0);
    CHECK(lut_phase_index(725.625) == 1);
}
