#include "ChipCheck.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace afar::cal {

namespace {

constexpr double kTieSlack = 1e-10;

double wrap_deg(double x)
{
    return x - 360.0 * std::floor((x + 180.0) / 360.0);
}

double mod360(double x)
{
    return x - 360.0 * std::floor(x / 360.0);
}

}  // namespace

bool ChipGrid::complete() const
{
    return std::all_of(present.begin(), present.end(), [](bool v) { return v; });
}

int ChipGrid::count() const
{
    return static_cast<int>(std::count(present.begin(), present.end(), true));
}

double hmc624a_state_limit_db(double atten_db, double freq_hz)
{
    // Табл. 1: 0,1–0,8 ГГц ±(0,1 + 5 % A), 0,8–6,0 ГГц ±(0,3 + 3 % A).
    if (freq_hz < 0.8e9) {
        return 0.1 + 0.05 * atten_db;
    }
    return 0.3 + 0.03 * atten_db;
}

AttenCheck check_attenuator(const ChipGrid& g)
{
    AttenCheck r;
    double sum2 = 0;
    double rangeSum = 0;
    double phaseSum = 0;
    int rangeCount = 0;
    for (int p = 0; p < kChipCodes; ++p) {
        const int ref = chip_state(p, kAttenZeroCode);
        if (!g.present[ref]) {
            continue;
        }
        double prevAtten = 0;
        bool havePrev = true;
        for (int a = kAttenZeroCode - 1; a >= 0; --a) {
            const int s = chip_state(p, a);
            if (!g.present[s]) {
                havePrev = false;
                continue;
            }
            const double nominal = nominal_atten_db(a);
            const double actual = g.gain_db[ref] - g.gain_db[s];
            const double e = actual - nominal;
            const double limit = hmc624a_state_limit_db(nominal, g.freq_hz);
            ++r.states_checked;
            sum2 += e * e;
            r.max_abs_state_error_db = std::max(r.max_abs_state_error_db, std::abs(e));
            if (std::abs(e) > limit + kTieSlack) {
                ++r.state_violations;
                r.max_excess_db = std::max(r.max_excess_db, std::abs(e) - limit);
            }
            if (havePrev) {
                const double step = std::abs(actual - prevAtten - 0.5);
                r.max_abs_step_error_db = std::max(r.max_abs_step_error_db, step);
                if (step > kHmcStepErrorTypDb + kTieSlack) {
                    ++r.step_over_typ;
                }
            }
            prevAtten = actual;
            havePrev = true;
        }
        const int full = chip_state(p, 0);
        if (g.present[full]) {
            rangeSum += g.gain_db[ref] - g.gain_db[full];
            const double rel = std::abs(wrap_deg(g.phase_deg[full] - g.phase_deg[ref]));
            phaseSum += rel;
            r.max_relative_phase_deg = std::max(r.max_relative_phase_deg, rel);
            ++rangeCount;
        }
    }
    if (r.states_checked > 0) {
        r.rms_state_error_db = std::sqrt(sum2 / r.states_checked);
    }
    if (rangeCount > 0) {
        r.mean_range_db = rangeSum / rangeCount;
        r.mean_relative_phase_deg = phaseSum / rangeCount;
    }
    return r;
}

PhaseCheck check_phase_shifter(const ChipGrid& g)
{
    PhaseCheck r;
    const int ref = chip_state(0, kAttenZeroCode);
    if (!g.present[ref]) {
        return r;
    }
    double sumPhase2 = 0;
    double sumAmp2 = 0;
    int n = 0;
    double gMin = g.gain_db[ref];
    double gMax = g.gain_db[ref];
    std::vector<double> shifts(kChipCodes, 0.0);
    for (int p = 1; p < kChipCodes; ++p) {
        const int s = chip_state(p, kAttenZeroCode);
        if (!g.present[s]) {
            continue;
        }
        // Сдвиг — уменьшение arg(S21) относительно p = 0, в [0, 360).
        const double shift = mod360(g.phase_deg[ref] - g.phase_deg[s]);
        shifts[static_cast<std::size_t>(p)] = shift;
        const double e = wrap_deg(shift - kPhaseLsbDeg * p);
        const double da = g.gain_db[s] - g.gain_db[ref];
        sumPhase2 += e * e;
        sumAmp2 += da * da;
        r.max_abs_phase_error_deg = std::max(r.max_abs_phase_error_deg, std::abs(e));
        gMin = std::min(gMin, g.gain_db[s]);
        gMax = std::max(gMax, g.gain_db[s]);
        ++n;
    }
    if (n > 0) {
        r.rms_phase_error_deg = std::sqrt(sumPhase2 / n);
        r.rms_amp_error_db = std::sqrt(sumAmp2 / n);
    }
    r.amp_peak_to_peak_db = gMax - gMin;
    for (std::size_t b = 0; b < kPhaseBitCode.size(); ++b) {
        const int p = kPhaseBitCode[b];
        const int s = chip_state(p, kAttenZeroCode);
        if (!g.present[s]) {
            continue;
        }
        r.bit_shift_deg[b] = shifts[static_cast<std::size_t>(p)];
        r.bit_error_deg[b] = wrap_deg(r.bit_shift_deg[b] - kPhaseBitNominalDeg[b]);
        r.bit_within_typ[b] = std::abs(r.bit_error_deg[b]) <= kPhaseBitTypDeg[b] + kTieSlack;
    }
    // Монотонность: развёрнутый вдоль кода сдвиг не убывает.
    double prev = 0;
    for (int p = 1; p < kChipCodes; ++p) {
        const double step = wrap_deg(shifts[static_cast<std::size_t>(p)] - prev);
        prev = shifts[static_cast<std::size_t>(p)];
        if (step <= 0) {
            r.monotonic = false;
        }
    }
    return r;
}

std::vector<double> choose_target_gains(const std::vector<const ChipGrid*>& grids, const LutOptions& options,
                                        double* g0_db)
{
    std::vector<double> gains;
    if (grids.empty() || !(options.gain_step_db > 0) || !std::isfinite(options.gain_margin_db)
        || options.gain_margin_db < 0) {
        return gains;
    }
    double top = std::numeric_limits<double>::infinity();
    double bottom = -std::numeric_limits<double>::infinity();
    for (const ChipGrid* g : grids) {
        for (int p = 0; p < kChipCodes; ++p) {
            top = std::min(top, g->gain_db[chip_state(p, kAttenZeroCode)]);
            bottom = std::max(bottom, g->gain_db[chip_state(p, 0)]);
        }
    }
    const double g0 = top - options.gain_margin_db;
    if (g0_db != nullptr) {
        *g0_db = g0;
    }
    if (!std::isfinite(g0) || !std::isfinite(bottom) || g0 < bottom) {
        return gains;
    }
    const int levels = static_cast<int>(std::floor((g0 - bottom) / options.gain_step_db + 1e-9)) + 1;
    gains.reserve(static_cast<std::size_t>(levels));
    for (int k = 0; k < levels; ++k) {
        gains.push_back(g0 - options.gain_step_db * k);
    }
    return gains;
}

FrequencyLut build_frequency_lut(const std::vector<const ChipGrid*>& grids, const LutOptions& options)
{
    FrequencyLut lut;
    if (grids.empty()) {
        lut.error = "нет каналов";
        return lut;
    }
    if (options.n_phase < 1) {
        lut.error = "n_phase должно быть ≥ 1";
        return lut;
    }
    if (options.use_tolerances
        && (!std::isfinite(options.gain_tol_db) || options.gain_tol_db < 0 || !std::isfinite(options.phase_tol_deg)
            || options.phase_tol_deg < 0 || options.phase_tol_deg > 180)) {
        lut.error = "допуски заданы неверно";
        return lut;
    }
    for (const ChipGrid* g : grids) {
        if (!g->complete()) {
            lut.error = "канал " + std::to_string(g->channel) + ": неполная сетка 64×64 — пропуски нулём не заполняются";
            return lut;
        }
        for (int s = 0; s < kChipStates; ++s) {
            if (!std::isfinite(g->gain_db[static_cast<std::size_t>(s)])
                || !std::isfinite(g->phase_deg[static_cast<std::size_t>(s)])) {
                lut.error = "канал " + std::to_string(g->channel) + ": неконечное измерение";
                return lut;
            }
        }
    }
    lut.freq_hz = grids.front()->freq_hz;
    // Одна общая фазовая опора для всех каналов (раздел 3): φ первого канала в (0, 63).
    lut.reference_phase_deg = wrap_deg(grids.front()->phase_deg[chip_state(0, kAttenZeroCode)]);
    lut.target_gains_db = choose_target_gains(grids, options, &lut.g0_db);
    if (lut.target_gains_db.empty()) {
        lut.error = "общий диапазон усиления каналов пуст";
        return lut;
    }
    const int nq = options.n_phase;
    lut.target_phases_deg.resize(static_cast<std::size_t>(nq));
    for (int q = 0; q < nq; ++q) {
        lut.target_phases_deg[static_cast<std::size_t>(q)] =
            wrap_deg(lut.reference_phase_deg + 360.0 * q / nq);
    }
    const std::size_t nk = lut.target_gains_db.size();
    std::vector<double> sin2(static_cast<std::size_t>(nq) * kChipStates);
    std::vector<double> dphi(sin2.size());
    std::vector<double> r(kChipStates);
    std::vector<double> dg(kChipStates);
    for (const ChipGrid* g : grids) {
        lut.channels.push_back(g->channel);
        std::vector<LutEntry> rows(nk * static_cast<std::size_t>(nq));
        for (int q = 0; q < nq; ++q) {
            for (int s = 0; s < kChipStates; ++s) {
                const std::size_t i = static_cast<std::size_t>(q) * kChipStates + static_cast<std::size_t>(s);
                dphi[i] = wrap_deg(g->phase_deg[static_cast<std::size_t>(s)] - lut.target_phases_deg[static_cast<std::size_t>(q)]);
                const double h = std::sin(dphi[i] * M_PI / 360.0);
                sin2[i] = h * h;
            }
        }
        for (std::size_t k = 0; k < nk; ++k) {
            const double gt = lut.target_gains_db[k];
            for (int s = 0; s < kChipStates; ++s) {
                dg[static_cast<std::size_t>(s)] = g->gain_db[static_cast<std::size_t>(s)] - gt;
                r[static_cast<std::size_t>(s)] = std::pow(10.0, dg[static_cast<std::size_t>(s)] / 20.0);
            }
            for (int q = 0; q < nq; ++q) {
                const double* s2 = sin2.data() + static_cast<std::size_t>(q) * kChipStates;
                const double* dp = dphi.data() + static_cast<std::size_t>(q) * kChipStates;
                int best = 0;
                double bestCost = std::numeric_limits<double>::infinity();
                int bestOk = -1;
                double bestOkCost = std::numeric_limits<double>::infinity();
                // Порядок s = 64·p + a: при равенстве выигрывает меньший p, затем меньший a.
                for (int s = 0; s < kChipStates; ++s) {
                    const double rr = r[static_cast<std::size_t>(s)];
                    const double cost = (rr - 1.0) * (rr - 1.0) + 4.0 * rr * s2[s];
                    if (cost < bestCost) {
                        bestCost = cost;
                        best = s;
                    }
                    if (options.use_tolerances && cost < bestOkCost
                        && std::abs(dg[static_cast<std::size_t>(s)]) <= options.gain_tol_db + kTieSlack
                        && std::abs(dp[s]) <= options.phase_tol_deg + kTieSlack) {
                        bestOkCost = cost;
                        bestOk = s;
                    }
                }
                LutEntry& e = rows[k * static_cast<std::size_t>(nq) + static_cast<std::size_t>(q)];
                int chosen = best;
                double chosenCost = bestCost;
                if (options.use_tolerances) {
                    e.status = bestOk >= 0 ? 1 : 0;
                    if (bestOk >= 0) {
                        chosen = bestOk;
                        chosenCost = bestOkCost;
                    }
                }
                e.phase_code = static_cast<std::uint8_t>(chosen / kChipCodes);
                e.att_code = static_cast<std::uint8_t>(chosen % kChipCodes);
                e.predicted_gain_db = g->gain_db[static_cast<std::size_t>(chosen)];
                e.predicted_phase_deg = wrap_deg(g->phase_deg[static_cast<std::size_t>(chosen)]);
                e.gain_error_db = dg[static_cast<std::size_t>(chosen)];
                e.phase_error_deg = dp[chosen];
                e.relative_complex_error = std::sqrt(chosenCost);
            }
        }
        lut.entries.push_back(std::move(rows));
    }
    return lut;
}

int lut_phase_index(double offset_deg, int n_phase)
{
    if (n_phase < 1 || !std::isfinite(offset_deg)) {
        return 0;
    }
    const double x = mod360(offset_deg);
    const auto q = static_cast<long long>(std::floor(x * n_phase / 360.0 + 0.5));
    return static_cast<int>(q % n_phase);
}

}  // namespace afar::cal
