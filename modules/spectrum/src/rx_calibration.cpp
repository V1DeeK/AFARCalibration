#include "rx_calibration.hpp"

#include "paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace {

constexpr double PI = 3.14159265358979323846;

// Позиция точки между соседними узлами сетки: индекс левого узла и вес правого.
// За границами сетки прижимается к краю — экстраполировать перекос не на чем,
// а тихо выдать мусор хуже, чем повторить крайнее измеренное значение.
void locate(const std::vector<double>& axis, double x, size_t& i0, size_t& i1, double& t) {
    if (axis.size() == 1 || x <= axis.front()) { i0 = i1 = 0; t = 0.0; return; }
    if (x >= axis.back()) { i0 = i1 = axis.size() - 1; t = 0.0; return; }

    size_t hi = static_cast<size_t>(
        std::lower_bound(axis.begin(), axis.end(), x) - axis.begin());
    i0 = hi - 1;
    i1 = hi;
    double span = axis[i1] - axis[i0];
    t = span > 0.0 ? (x - axis[i0]) / span : 0.0;
}

} // namespace

bool RxCalibration::load(const std::string& path, std::string& error) {
    std::ifstream f(resolve_data_path(path));
    if (!f) {
        error = "cannot open " + path;
        return false;
    }

    // Сначала собираем в map: файл обязан быть полным прямоугольником, но
    // порядок строк не оговорён, и проверить полноту можно только собрав всё.
    struct Point { float phase, amp, delay; };
    std::map<double, std::map<double, Point>> grid;
    std::string line;
    size_t line_no = 0, parsed = 0;
    bool   has_delay = false;

    while (std::getline(f, line)) {
        ++line_no;
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("freq_hz", 0) == 0) {
            // Пятая колонка есть в обоих форматах, но в v1 это phase_raw_deg, а
            // в v2 — delay_ns. Читать её по позиции, не глядя на заголовок,
            // значит принять несглаженную фазу за задержку в наносекундах.
            has_delay = line.find(",delay_ns,") != std::string::npos;
            continue;
        }

        double hz = 0.0, gain = 0.0;
        float  phase = 0.0f, amp = 0.0f, delay = 0.0f;
        // Дальше в строке могут быть *_raw колонки — они здесь не нужны, и
        // sscanf их просто не читает.
        int want = has_delay ? 5 : 4;
        int got  = has_delay
            ? std::sscanf(line.c_str(), "%lf,%lf,%f,%f,%f", &hz, &gain, &phase, &amp, &delay)
            : std::sscanf(line.c_str(), "%lf,%lf,%f,%f", &hz, &gain, &phase, &amp);
        if (got != want) {
            error = "malformed line " + std::to_string(line_no) + " in " + path;
            return false;
        }
        grid[hz][gain] = { phase, amp, delay };
        ++parsed;
    }

    if (grid.empty()) {
        error = path + " has no data rows";
        return false;
    }

    std::vector<double> freqs, gains;
    freqs.reserve(grid.size());
    for (const auto& [hz, row] : grid) freqs.push_back(hz);
    for (const auto& [gain, _] : grid.begin()->second) gains.push_back(gain);

    // Неполный прямоугольник сделал бы интерполяцию по усилению неверной ровно
    // на тех частотах, где точки не хватает, и молча.
    for (const auto& [hz, row] : grid) {
        if (row.size() != gains.size()) {
            error = "gain grid is not the same at every frequency (" +
                    std::to_string(static_cast<long long>(hz / 1000000)) + " MHz has " +
                    std::to_string(row.size()) + " of " + std::to_string(gains.size()) + ")";
            return false;
        }
    }

    std::vector<float> phase(freqs.size() * gains.size());
    std::vector<float> amp(phase.size());
    std::vector<float> delay(phase.size());
    size_t fi = 0;
    for (const auto& [hz, row] : grid) {
        size_t gi = 0;
        for (const auto& [gain, value] : row) {
            phase[fi * gains.size() + gi] = value.phase;
            amp  [fi * gains.size() + gi] = value.amp;
            delay[fi * gains.size() + gi] = value.delay;
            ++gi;
        }
        ++fi;
    }

    freqs_     = std::move(freqs);
    gains_     = std::move(gains);
    phase_     = std::move(phase);
    amp_       = std::move(amp);
    delay_     = std::move(delay);
    has_delay_ = has_delay;

    std::ostringstream src;
    src << path << ": " << parsed << " points, "
        << freqs_.front() / 1e6 << ".." << freqs_.back() / 1e6 << " MHz, "
        << gains_.front() << ".." << gains_.back() << " dB"
        << (has_delay_ ? ", with delay" : ", no delay column");
    source_ = src.str();
    return true;
}

double RxCalibration::interpolate(const std::vector<float>& values,
                                  double freq_hz, double gain_db) const {
    size_t f0, f1, g0, g1;
    double tf, tg;
    locate(freqs_, freq_hz, f0, f1, tf);
    locate(gains_, gain_db, g0, g1, tg);

    auto at = [&](size_t fi, size_t gi) {
        return static_cast<double>(values[fi * gains_.size() + gi]);
    };
    double a = at(f0, g0) * (1 - tg) + at(f0, g1) * tg;
    double b = at(f1, g0) * (1 - tg) + at(f1, g1) * tg;
    return a * (1 - tf) + b * tf;
}

bool RxCalibration::imbalance(double freq_hz, double gain_db,
                              float& phase_deg, float& amp_db) const {
    if (!valid()) return false;

    auto bilinear = [&](const std::vector<float>& v) {
        return interpolate(v, freq_hz, gain_db);
    };

    // Фаза интерполируется как обычное число, а не как вектор: соседние узлы
    // сетки отличаются на доли градуса (шаг 10 МГц при периоде ряби 158 МГц),
    // так что через -180/+180 здесь не перескакивает. Если таблицу когда-нибудь
    // снимут на грубой сетке, это место придётся считать через комплексные веса.
    phase_deg = static_cast<float>(bilinear(phase_));
    amp_db    = static_cast<float>(bilinear(amp_));
    return true;
}

bool RxCalibration::in_range(double freq_hz, double gain_db) const {
    return valid()
        && freq_hz >= freqs_.front() && freq_hz <= freqs_.back()
        && gain_db >= gains_.front() && gain_db <= gains_.back();
}

float RxCalibration::delay_ns(double freq_hz, double gain_db) const {
    if (!valid() || !has_delay_) return 0.0f;
    return static_cast<float>(interpolate(delay_, freq_hz, gain_db));
}

std::complex<float> RxCalibration::correction(double freq_hz, double gain_db) const {
    return correction(freq_hz, gain_db, 0.0);
}

std::complex<float> RxCalibration::correction(double freq_hz, double gain_db,
                                              double offset_hz) const {
    float phase_deg = 0.0f, amp_db = 0.0f;
    if (!imbalance(freq_hz, gain_db, phase_deg, amp_db)) return { 1.0f, 0.0f };

    // Задержка добавляет к перекосу фазу, линейную по смещению бина от
    // гетеродина: phi = phase + 360 * f_off * tau. На нулевом смещении и на
    // таблице v1 это слагаемое равно нулю, поэтому отдельной ветки нет.
    double total_deg = phase_deg + 360.0 * offset_hz * delay_ns(freq_hz, gain_db) * 1e-9;

    // Таблица хранит перекос arg(Rx2/Rx1); чтобы его снять, Rx2 домножается на
    // обратную величину.
    float scale = std::pow(10.0f, -amp_db / 20.0f);
    float rad   = -static_cast<float>(total_deg * PI / 180.0);
    return { scale * std::cos(rad), scale * std::sin(rad) };
}
