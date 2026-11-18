#include "BeamCodes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace {

constexpr double kLight = 299792458.0;
constexpr double kLsbDeg = 360.0 / 64.0;
constexpr int kCodes = 64;
constexpr int kStates = kCodes * kCodes;
constexpr double kLimitDeg = 30.0;

struct Command {
    double angleDeg = 0;
    double betaDeg = 0;
    int k = -1;
    bool edge = false;
};

struct Grid {
    std::array<float, kStates> gain{};
    std::array<float, kStates> phase{};
    std::array<char, kStates> seen{};
};

double wrapDeg(double angle)
{
    double y = std::fmod(angle + 180.0, 360.0);
    if (y < 0.0) {
        y += 360.0;
    }
    return y - 180.0;
}

std::vector<Command> scanCommands(double freqHz, double pitchM)
{
    const double scale = 360.0 * pitchM * freqHz / kLight;
    const double edge = scale * std::sin(kLimitDeg * std::acos(-1.0) / 180.0);
    const int last = static_cast<int>(std::floor(edge / kLsbDeg + 1e-12));
    std::vector<Command> commands;
    commands.reserve(static_cast<std::size_t>(last * 2 + 4));
    for (int k = -last; k <= last; ++k) {
        const double s = std::clamp(k * kLsbDeg / scale, -1.0, 1.0);
        commands.push_back(Command{std::asin(s) * 180.0 / std::acos(-1.0), -k * kLsbDeg, k, false});
    }
    if (std::abs(edge - last * kLsbDeg) > 1e-9) {
        commands.push_back(Command{-kLimitDeg, edge, -1, true});
        commands.push_back(Command{kLimitDeg, -edge, -1, true});
    }
    std::sort(commands.begin(), commands.end(), [](const Command& a, const Command& b) {
        return a.angleDeg < b.angleDeg;
    });
    return commands;
}

std::vector<int> circleCandidates(const std::vector<std::complex<double>>& states)
{
    std::vector<int> ids;
    if (states.empty()) {
        return ids;
    }
    double worst = 0;
    for (int probe = 0; probe < 64; ++probe) {
        const auto p = std::polar(1.0, 2.0 * std::acos(-1.0) * probe / 64.0);
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto& state : states) {
            nearest = std::min(nearest, std::abs(p - state));
        }
        worst = std::max(worst, nearest);
    }
    const double bound = worst + 2.0 * std::sin(std::acos(-1.0) / 128.0) + 1e-12;
    for (int i = 0; i < static_cast<int>(states.size()); ++i) {
        if (std::abs(std::abs(states[static_cast<std::size_t>(i)]) - 1.0) <= bound) {
            ids.push_back(i);
        }
    }
    return ids;
}

struct Solved {
    double common = 0;
    std::vector<int> phase;
    std::vector<int> atten;
    std::vector<double> gainError;
    std::vector<double> phaseError;
    std::vector<double> benchGain;
    std::vector<double> benchPhase;
    std::vector<double> targetPhase;
    double maxAbsGain = 0;
    double maxAbsPhase = 0;
    double maxPairGain = 0;
    double maxPairPhase = 0;
};

Solved solveBeam(const std::vector<Grid>& grids, const std::vector<double>& psi, double targetGain)
{
    const int n = static_cast<int>(grids.size());
    std::vector<std::vector<std::complex<double>>> z(static_cast<std::size_t>(n));
    std::vector<std::vector<int>> indices(static_cast<std::size_t>(n));
    for (int ch = 0; ch < n; ++ch) {
        z[static_cast<std::size_t>(ch)].resize(kStates);
        for (int s = 0; s < kStates; ++s) {
            const double amp = std::pow(10.0, (grids[static_cast<std::size_t>(ch)].gain[static_cast<std::size_t>(s)] - targetGain) / 20.0);
            const double rel = (grids[static_cast<std::size_t>(ch)].phase[static_cast<std::size_t>(s)] - psi[static_cast<std::size_t>(ch)]) * std::acos(-1.0) / 180.0;
            z[static_cast<std::size_t>(ch)][static_cast<std::size_t>(s)] = std::polar(amp, rel);
        }
        indices[static_cast<std::size_t>(ch)] = circleCandidates(z[static_cast<std::size_t>(ch)]);
    }
    constexpr int kGrid = static_cast<int>(360.0 / 0.125);
    std::vector<double> cost(kGrid, 0);
    for (int ch = 0; ch < n; ++ch) {
        const auto& ids = indices[static_cast<std::size_t>(ch)];
        for (int g = 0; g < kGrid; ++g) {
            const double angle = -180.0 + g * 0.125;
            const auto unit = std::polar(1.0, angle * std::acos(-1.0) / 180.0);
            double best = std::numeric_limits<double>::infinity();
            for (const int id : ids) {
                const auto d = unit - z[static_cast<std::size_t>(ch)][static_cast<std::size_t>(id)];
                best = std::min(best, d.real() * d.real() + d.imag() * d.imag());
            }
            cost[static_cast<std::size_t>(g)] += best;
        }
    }
    const int bestGrid = static_cast<int>(std::min_element(cost.begin(), cost.end()) - cost.begin());
    auto choose = [&](double angle) {
        const auto unit = std::polar(1.0, angle * std::acos(-1.0) / 180.0);
        std::vector<int> selected(static_cast<std::size_t>(n));
        for (int ch = 0; ch < n; ++ch) {
            const auto& ids = indices[static_cast<std::size_t>(ch)];
            double best = std::numeric_limits<double>::infinity();
            int pick = ids.empty() ? 0 : ids.front();
            for (const int id : ids) {
                const auto d = z[static_cast<std::size_t>(ch)][static_cast<std::size_t>(id)] - unit;
                const double cost2 = d.real() * d.real() + d.imag() * d.imag();
                if (cost2 < best) {
                    best = cost2;
                    pick = id;
                }
            }
            selected[static_cast<std::size_t>(ch)] = pick;
        }
        return selected;
    };
    double common = -180.0 + bestGrid * 0.125;
    auto selected = choose(common);
    for (int pass = 0; pass < 16; ++pass) {
        std::complex<double> summed;
        for (int ch = 0; ch < n; ++ch) {
            summed += z[static_cast<std::size_t>(ch)][static_cast<std::size_t>(selected[static_cast<std::size_t>(ch)])];
        }
        if (std::abs(summed) < 1e-14) {
            break;
        }
        common = wrapDeg(std::arg(summed) * 180.0 / std::acos(-1.0));
        auto updated = choose(common);
        if (updated == selected) {
            break;
        }
        selected = std::move(updated);
    }
    Solved out;
    out.common = common;
    out.phase.resize(static_cast<std::size_t>(n));
    out.atten.resize(static_cast<std::size_t>(n));
    out.gainError.resize(static_cast<std::size_t>(n));
    out.phaseError.resize(static_cast<std::size_t>(n));
    out.benchGain.resize(static_cast<std::size_t>(n));
    out.benchPhase.resize(static_cast<std::size_t>(n));
    out.targetPhase.resize(static_cast<std::size_t>(n));
    for (int ch = 0; ch < n; ++ch) {
        const int s = selected[static_cast<std::size_t>(ch)];
        const int p = s / kCodes;
        const int a = s % kCodes;
        out.phase[static_cast<std::size_t>(ch)] = p;
        out.atten[static_cast<std::size_t>(ch)] = a;
        const double g = grids[static_cast<std::size_t>(ch)].gain[static_cast<std::size_t>(s)];
        const double ph = grids[static_cast<std::size_t>(ch)].phase[static_cast<std::size_t>(s)];
        out.benchGain[static_cast<std::size_t>(ch)] = g;
        out.benchPhase[static_cast<std::size_t>(ch)] = ph;
        out.targetPhase[static_cast<std::size_t>(ch)] = wrapDeg(common + psi[static_cast<std::size_t>(ch)]);
        out.gainError[static_cast<std::size_t>(ch)] = g - targetGain;
        out.phaseError[static_cast<std::size_t>(ch)] = wrapDeg(ph - out.targetPhase[static_cast<std::size_t>(ch)]);
        out.maxAbsGain = std::max(out.maxAbsGain, std::abs(out.gainError[static_cast<std::size_t>(ch)]));
        out.maxAbsPhase = std::max(out.maxAbsPhase, std::abs(out.phaseError[static_cast<std::size_t>(ch)]));
    }
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            out.maxPairGain = std::max(out.maxPairGain, std::abs(out.gainError[static_cast<std::size_t>(i)] - out.gainError[static_cast<std::size_t>(j)]));
            out.maxPairPhase = std::max(out.maxPairPhase, std::abs(wrapDeg(out.phaseError[static_cast<std::size_t>(i)] - out.phaseError[static_cast<std::size_t>(j)])));
        }
    }
    return out;
}

std::vector<std::string> splitCsv(const std::string& line)
{
    std::vector<std::string> cells;
    std::string cell;
    for (char ch : line) {
        if (ch == ',') {
            cells.push_back(cell);
            cell.clear();
        } else if (ch != '\r') {
            cell.push_back(ch);
        }
    }
    cells.push_back(cell);
    return cells;
}

std::string loadChannel(const std::string& path, int channelId, std::map<std::int64_t, Grid>& byFreq)
{
    std::ifstream in(path);
    if (!in) {
        return "не открыт " + path;
    }
    std::string header;
    if (!std::getline(in, header)) {
        return "пустой " + path;
    }
    if (header.size() >= 3 && static_cast<unsigned char>(header[0]) == 0xEF) {
        header.erase(0, 3);
    }
    const auto cols = splitCsv(header);
    auto index = [&](const char* name) {
        for (int i = 0; i < static_cast<int>(cols.size()); ++i) {
            if (cols[static_cast<std::size_t>(i)] == name) {
                return i;
            }
        }
        return -1;
    };
    const int cCh = index("channel_id");
    const int cF = index("freq_hz");
    const int cP = index("phase_code");
    const int cA = index("att_code");
    const int cG = index("gain_db");
    const int cPh = index("phase_deg");
    if (cCh < 0 || cF < 0 || cP < 0 || cA < 0 || cG < 0 || cPh < 0) {
        return "в CSV нет столбцов прохода: " + path;
    }
    const int need = std::max({cCh, cF, cP, cA, cG, cPh});
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        const auto cells = splitCsv(line);
        if (static_cast<int>(cells.size()) <= need) {
            continue;
        }
        try {
            if (std::stoi(cells[static_cast<std::size_t>(cCh)]) != channelId) {
                continue;
            }
            const auto freq = static_cast<std::int64_t>(std::llround(std::stod(cells[static_cast<std::size_t>(cF)])));
            const int p = std::stoi(cells[static_cast<std::size_t>(cP)]);
            const int a = std::stoi(cells[static_cast<std::size_t>(cA)]);
            if (p < 0 || p >= kCodes || a < 0 || a >= kCodes) {
                return path + ": код вне 0…63";
            }
            const int s = p * kCodes + a;
            auto& grid = byFreq[freq];
            if (grid.seen[static_cast<std::size_t>(s)]) {
                return path + ": повтор состояния";
            }
            grid.seen[static_cast<std::size_t>(s)] = 1;
            grid.gain[static_cast<std::size_t>(s)] = static_cast<float>(std::stod(cells[static_cast<std::size_t>(cG)]));
            grid.phase[static_cast<std::size_t>(s)] = static_cast<float>(std::stod(cells[static_cast<std::size_t>(cPh)]));
        } catch (const std::exception&) {
            continue;
        }
    }
    return {};
}

} // namespace

std::string writeBeamCodeTables(const BeamTableRequest& request, const std::function<void(const std::string&)>& progress)
{
    if (request.measurementsCsv.size() != 16) {
        return "нужны 16 файлов measurements.csv: 4 изделия по 4 канала";
    }
    if (!(request.stepHz > 0) || request.stopHz < request.startHz || !(request.targetGainDb > -100)) {
        return "полоса или цель по усилению заданы неверно";
    }
    std::vector<std::map<std::int64_t, Grid>> channels(16);
    for (int i = 0; i < 16; ++i) {
        if (progress) {
            progress("читаю канал " + std::to_string(i + 1));
        }
        const std::string error = loadChannel(request.measurementsCsv[static_cast<std::size_t>(i)], i % 4, channels[static_cast<std::size_t>(i)]);
        if (!error.empty()) {
            return error;
        }
    }
    std::vector<std::int64_t> freqs;
    for (auto it = channels.front().begin(); it != channels.front().end(); ++it) {
        const double hz = static_cast<double>(it->first);
        if (hz + 0.5 < request.startHz || hz - 0.5 > request.stopHz) {
            continue;
        }
        const double steps = (hz - request.startHz) / request.stepHz;
        if (std::abs(steps - std::round(steps)) > 1e-3) {
            continue;
        }
        bool ready = true;
        for (const auto& channel : channels) {
            const auto found = channel.find(it->first);
            if (found == channel.end() || std::count(found->second.seen.begin(), found->second.seen.end(), 1) != kStates) {
                ready = false;
                break;
            }
        }
        if (ready) {
            freqs.push_back(it->first);
        }
    }
    if (freqs.empty()) {
        return "нет ни одной частоты, где у всех 16 каналов полные 64×64";
    }
    const struct Antenna { const char* name; double pitchM; } antennas[] = {{"air", 0.0372}, {"rogers", 0.0327}};
    for (const Antenna& antenna : antennas) {
        const std::string common = request.outputDir + "/scan_" + antenna.name + "_all.csv";
        std::vector<std::ofstream> products;
        std::ofstream all(common);
        if (!all) {
            return "не удалось записать " + common;
        }
        auto headerCodes = [](int count) {
            std::ostringstream s;
            for (int i = 1; i <= count; ++i) {
                s << ",ch" << i << "_phase_code,ch" << i << "_att_code";
            }
            return s.str();
        };
        all << "\xEF\xBB\xBF" << "antenna,freq_hz,pitch_mm,command_id,kind,slope_lsb_index,azimuth_deg,adjacent_phase_deg,target_gain_db,bench_common_phase_deg"
            << headerCodes(16) << "\n";
        for (int product = 0; product < 4; ++product) {
            products.emplace_back(request.outputDir + "/scan_" + antenna.name + "_product" + std::to_string(product + 1) + ".csv");
            if (!products.back()) {
                return "не удалось записать файл изделия";
            }
            products.back() << "\xEF\xBB\xBF" << "product,antenna,freq_hz,pitch_mm,command_id,kind,slope_lsb_index,azimuth_deg,adjacent_phase_deg,target_gain_db,bench_common_phase_deg"
                            << headerCodes(4) << "\n";
        }
        for (const std::int64_t freq : freqs) {
            if (progress) {
                progress(std::string(antenna.name) + " " + std::to_string(freq / 1000000) + " МГц");
            }
            std::vector<Grid> grids;
            grids.reserve(16);
            for (const auto& channel : channels) {
                grids.push_back(channel.at(freq));
            }
            const auto commands = scanCommands(static_cast<double>(freq), antenna.pitchM);
            int commandId = 1;
            for (const Command& command : commands) {
                std::vector<double> psi(16);
                for (int column = 0; column < 16; ++column) {
                    psi[static_cast<std::size_t>(column)] = wrapDeg(column * command.betaDeg);
                }
                const Solved row = solveBeam(grids, psi, request.targetGainDb);
                auto prefix = [&](std::ostream& out) {
                    out << antenna.name << ',' << freq << ',' << (antenna.pitchM * 1000.0) << ',' << commandId << ','
                        << (command.edge ? "edge" : "lsb") << ',';
                    if (command.edge) {
                        out << ',';
                    } else {
                        out << command.k << ',';
                    }
                    out << command.angleDeg << ',' << command.betaDeg << ',' << request.targetGainDb << ',' << row.common;
                };
                prefix(all);
                for (int ch = 0; ch < 16; ++ch) {
                    all << ',' << row.phase[static_cast<std::size_t>(ch)] << ',' << row.atten[static_cast<std::size_t>(ch)];
                }
                all << '\n';
                for (int product = 0; product < 4; ++product) {
                    products[static_cast<std::size_t>(product)] << (product + 1) << ',';
                    prefix(products[static_cast<std::size_t>(product)]);
                    for (int ch = product * 4; ch < product * 4 + 4; ++ch) {
                        products[static_cast<std::size_t>(product)] << ',' << row.phase[static_cast<std::size_t>(ch)] << ',' << row.atten[static_cast<std::size_t>(ch)];
                    }
                    products[static_cast<std::size_t>(product)] << '\n';
                }
                ++commandId;
            }
        }
    }
    if (progress) {
        progress("готово, частот " + std::to_string(freqs.size()));
    }
    return {};
}
