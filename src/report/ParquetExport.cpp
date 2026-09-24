#include "ParquetExport.h"

#include "InverseLut.h"
#include "Normalize.h"
#include "PhaseMath.h"

#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace afar::report {
namespace {

bool writeMagicAndTsv(const std::filesystem::path& path,
                      const std::string& tsv,
                      std::string& diagnostics)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        diagnostics = "cannot open for write: " + path.string();
        return false;
    }
    out.write(kAfarPqMagic, static_cast<std::streamsize>(sizeof(kAfarPqMagic)));
    out.write(tsv.data(), static_cast<std::streamsize>(tsv.size()));
    if (!out) {
        diagnostics = "write failed: " + path.string();
        return false;
    }
    return true;
}

bool readMagicAndTsv(const std::filesystem::path& path,
                     std::string& tsv,
                     std::string& diagnostics)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot open for read: " + path.string();
        return false;
    }
    char magic[sizeof(kAfarPqMagic)]{};
    in.read(magic, static_cast<std::streamsize>(sizeof(magic)));
    if (!in || std::memcmp(magic, kAfarPqMagic, sizeof(kAfarPqMagic)) != 0) {
        diagnostics = "bad AFARPQ magic: " + path.string();
        return false;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    tsv = oss.str();
    return true;
}

std::vector<std::string_view> splitTsvLine(std::string_view line)
{
    std::vector<std::string_view> cols;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto tab = line.find('\t', start);
        if (tab == std::string_view::npos) {
            cols.push_back(line.substr(start));
            break;
        }
        cols.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return cols;
}

bool parseBool(std::string_view s, bool& out)
{
    if (s == "1" || s == "true" || s == "True") {
        out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "False") {
        out = false;
        return true;
    }
    return false;
}

template <typename T>
bool parseNum(std::string_view s, T& out)
{
    try {
        if constexpr (std::is_same_v<T, double>) {
            out = std::stod(std::string(s));
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            out = static_cast<T>(std::stoull(std::string(s)));
        } else if constexpr (std::is_same_v<T, std::uint16_t>) {
            out = static_cast<T>(std::stoul(std::string(s)));
        } else if constexpr (std::is_same_v<T, std::uint8_t>) {
            out = static_cast<T>(std::stoul(std::string(s)));
        } else {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::string formatDouble(double v)
{
    if (!std::isfinite(v)) {
        return "nan";
    }
    std::ostringstream oss;
    oss << std::setprecision(17) << v;
    return oss.str();
}

std::optional<std::uint32_t> referenceAttRow(const RawS21Store& store, std::uint16_t att_code)
{
    const auto& atts = store.attCodes();
    for (std::uint32_t i = 0; i < atts.size(); ++i) {
        if (atts[i] == att_code) {
            return i;
        }
    }
    return std::nullopt;
}

bool is_measured_sample(std::complex<double> z)
{
    if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) {
        return false;
    }
    const double mag2 = z.real() * z.real() + z.imag() * z.imag();
    return mag2 > 0.0 && std::isfinite(mag2);
}

/// Строки опоры 0…N_A−1. Слот N_A (лишний) не читается: он нулевой и не измерение.
std::vector<std::vector<std::complex<double>>> loadMeasuredReferenceRows(
    const RawS21Store& store,
    std::uint8_t channel)
{
    std::vector<std::vector<std::complex<double>>> refs(store.nAtt());
    for (std::uint32_t row = 0; row < store.nAtt(); ++row) {
        std::string diag;
        if (!store.readReference(channel, row, refs[row], diag)) {
            refs[row].assign(store.nFreq(), {0.0, 0.0});
        }
    }
    return refs;
}

double driftAgainstPreviousReference(
    const std::vector<std::vector<std::complex<double>>>& refs,
    std::uint32_t row,
    std::size_t freq_index)
{
    if (row >= refs.size() || freq_index >= refs[row].size()
        || !is_measured_sample(refs[row][freq_index])) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    std::uint32_t prev = row;
    while (prev > 0) {
        --prev;
        if (freq_index >= refs[prev].size() || !is_measured_sample(refs[prev][freq_index])) {
            continue;
        }
        const auto drift =
            cal::reference_drift_phase_deg(refs[prev][freq_index], refs[row][freq_index]);
        if (!drift) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        return *drift;
    }
    return std::numeric_limits<double>::quiet_NaN();
}

}  // namespace

bool exportDirectLut(const std::filesystem::path& path,
                     std::span<const cal::DirectLutEntry> rows,
                     std::string& diagnostics)
{
    std::ostringstream tsv;
    tsv << "channel\tfreq_hz\tatt_code\tphase_code\ts21_re\ts21_im\tmag_db\t"
           "phase_unwrapped_deg\tatten_meas_db\tphase_error_deg\tdrift_phase_deg\t"
           "repeatability_db\trepeatability_deg\tvalid\n";
    for (const auto& e : rows) {
        tsv << static_cast<unsigned>(e.channel) << '\t' << e.freq_hz << '\t' << e.att_code
            << '\t' << static_cast<unsigned>(e.phase_code) << '\t' << formatDouble(e.s21_re)
            << '\t' << formatDouble(e.s21_im) << '\t' << formatDouble(e.mag_db) << '\t'
            << formatDouble(e.phase_unwrapped_deg) << '\t' << formatDouble(e.atten_meas_db)
            << '\t' << formatDouble(e.phase_error_deg) << '\t' << formatDouble(e.drift_phase_deg)
            << '\t' << formatDouble(e.repeatability_db) << '\t'
            << formatDouble(e.repeatability_deg) << '\t' << (e.valid ? 1 : 0) << '\n';
    }
    return writeMagicAndTsv(path, tsv.str(), diagnostics);
}

bool exportInverseLut(const std::filesystem::path& path,
                      std::span<const InverseLutEntry> rows,
                      std::string& diagnostics)
{
    std::ostringstream tsv;
    tsv << "channel\tfreq_hz\ttarget_atten_db\ttarget_phase_deg\tselected_att_code\t"
           "selected_phase_code\tmeasured_atten_db\tmeasured_phase_deg\t"
           "atten_residual_db\tphase_residual_deg\tvalid\n";
    for (const auto& e : rows) {
        tsv << static_cast<unsigned>(e.channel) << '\t' << e.freq_hz << '\t'
            << formatDouble(e.target_atten_db) << '\t' << formatDouble(e.target_phase_deg)
            << '\t' << e.selected_att_code << '\t' << static_cast<unsigned>(e.selected_phase_code)
            << '\t' << formatDouble(e.measured_atten_db) << '\t'
            << formatDouble(e.measured_phase_deg) << '\t' << formatDouble(e.atten_residual_db)
            << '\t' << formatDouble(e.phase_residual_deg) << '\t' << (e.valid ? 1 : 0) << '\n';
    }
    return writeMagicAndTsv(path, tsv.str(), diagnostics);
}

bool readDirectLut(const std::filesystem::path& path,
                   std::vector<cal::DirectLutEntry>& out,
                   std::string& diagnostics)
{
    out.clear();
    std::string tsv;
    if (!readMagicAndTsv(path, tsv, diagnostics)) {
        return false;
    }
    std::istringstream iss(tsv);
    std::string line;
    if (!std::getline(iss, line)) {
        diagnostics = "empty direct lut tsv";
        return false;
    }
    // strip CR
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.find("channel") == std::string::npos || line.find("freq_hz") == std::string::npos) {
        diagnostics = "unexpected direct lut header";
        return false;
    }
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto cols = splitTsvLine(line);
        if (cols.size() < 14) {
            diagnostics = "direct lut row too short";
            return false;
        }
        cal::DirectLutEntry e;
        bool ok = parseNum(cols[0], e.channel) && parseNum(cols[1], e.freq_hz)
            && parseNum(cols[2], e.att_code) && parseNum(cols[3], e.phase_code)
            && parseNum(cols[4], e.s21_re) && parseNum(cols[5], e.s21_im)
            && parseNum(cols[6], e.mag_db) && parseNum(cols[7], e.phase_unwrapped_deg)
            && parseNum(cols[8], e.atten_meas_db) && parseNum(cols[9], e.phase_error_deg)
            && parseNum(cols[10], e.drift_phase_deg) && parseNum(cols[11], e.repeatability_db)
            && parseNum(cols[12], e.repeatability_deg) && parseBool(cols[13], e.valid);
        if (!ok) {
            diagnostics = "direct lut parse error";
            return false;
        }
        out.push_back(e);
    }
    return true;
}

bool readInverseLut(const std::filesystem::path& path,
                    std::vector<InverseLutEntry>& out,
                    std::string& diagnostics)
{
    out.clear();
    std::string tsv;
    if (!readMagicAndTsv(path, tsv, diagnostics)) {
        return false;
    }
    std::istringstream iss(tsv);
    std::string line;
    if (!std::getline(iss, line)) {
        diagnostics = "empty inverse lut tsv";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line.find("target_atten_db") == std::string::npos) {
        diagnostics = "unexpected inverse lut header";
        return false;
    }
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto cols = splitTsvLine(line);
        if (cols.size() < 11) {
            diagnostics = "inverse lut row too short";
            return false;
        }
        InverseLutEntry e;
        bool ok = parseNum(cols[0], e.channel) && parseNum(cols[1], e.freq_hz)
            && parseNum(cols[2], e.target_atten_db) && parseNum(cols[3], e.target_phase_deg)
            && parseNum(cols[4], e.selected_att_code) && parseNum(cols[5], e.selected_phase_code)
            && parseNum(cols[6], e.measured_atten_db) && parseNum(cols[7], e.measured_phase_deg)
            && parseNum(cols[8], e.atten_residual_db) && parseNum(cols[9], e.phase_residual_deg)
            && parseBool(cols[10], e.valid);
        if (!ok) {
            diagnostics = "inverse lut parse error";
            return false;
        }
        out.push_back(e);
    }
    return true;
}

bool buildDirectLutFromStore(const RawS21Store& store,
                             const RunConfig& config,
                             std::vector<cal::DirectLutEntry>& out,
                             std::string& diagnostics)
{
    out.clear();
    const double lsb = config.dut.phase_codes.lsb_deg;
    const double drift_limit = config.limits.max_drift_phase_deg;
    for (const auto ch : store.channels()) {
        const auto refs = loadMeasuredReferenceRows(store, ch);
        for (const auto att : store.attCodes()) {
            for (const auto ph : store.phaseCodes()) {
                if (!store.isCompleted(ch, att, ph)) {
                    continue;
                }
                RawS21StateRecord rec;
                if (!store.readState(ch, att, ph, rec, diagnostics)) {
                    return false;
                }
                const auto row = referenceAttRow(store, att);
                std::vector<std::complex<double>> ref;
                if (row) {
                    if (!store.readReference(ch, *row, ref, diagnostics)) {
                        // нет опоры — нули → нормализация даст invalid
                        ref.assign(store.nFreq(), {0.0, 0.0});
                        diagnostics.clear();
                    }
                } else {
                    ref.assign(store.nFreq(), {0.0, 0.0});
                }
                if (ref.size() != rec.s21.size()) {
                    diagnostics = "reference/state length mismatch";
                    return false;
                }
                const auto norm = cal::normalize_sweep(rec.s21, ref);
                std::vector<std::complex<double>> s_tilde(norm.size());
                for (std::size_t i = 0; i < norm.size(); ++i) {
                    s_tilde[i] = norm[i].s_tilde;
                }
                const auto unwrapped = cal::unwrap_phase_deg(s_tilde);
                const double nominal = static_cast<double>(ph) * lsb;
                for (std::size_t fi = 0; fi < store.nFreq(); ++fi) {
                    cal::DirectLutBuildInput in;
                    in.channel = ch;
                    in.freq_hz = store.frequencyHz()[fi];
                    in.att_code = att;
                    in.phase_code = ph;
                    in.s_tilde = (fi < s_tilde.size()) ? s_tilde[fi] : std::complex<double>{};
                    in.phase_unwrapped_deg =
                        (fi < unwrapped.size()) ? unwrapped[fi] : 0.0;
                    in.nominal_phase_deg = nominal;
                    const auto att_row = referenceAttRow(store, att);
                    in.drift_phase_deg = att_row
                        ? driftAgainstPreviousReference(refs, *att_row, fi)
                        : std::numeric_limits<double>::quiet_NaN();
                    // В слоте хранится только последний свип: истории повторных attempt нет.
                    const auto repeatability = cal::repeatability_from_attempts({});
                    if (repeatability) {
                        in.repeatability_db = repeatability->db;
                        in.repeatability_deg = repeatability->deg;
                    } else {
                        in.repeatability_db = std::numeric_limits<double>::quiet_NaN();
                        in.repeatability_deg = std::numeric_limits<double>::quiet_NaN();
                    }
                    in.sample_valid = (fi < rec.valid.size()) && (rec.valid[fi] != 0)
                        && (fi < norm.size()) && norm[fi].valid && !rec.overload;
                    if (std::isfinite(in.drift_phase_deg)
                        && std::fabs(in.drift_phase_deg) > drift_limit) {
                        in.sample_valid = false;
                    }
                    out.push_back(cal::build_direct_lut_entry(in));
                }
            }
        }
    }
    return true;
}

bool buildInverseLutFromDirect(const std::vector<cal::DirectLutEntry>& direct,
                               const RunConfig& config,
                               const AttenuatorCodes& att,
                               std::vector<InverseLutEntry>& out,
                               std::string& diagnostics)
{
    out.clear();
    (void)diagnostics;

    struct Key {
        std::uint8_t channel{};
        std::uint64_t freq_hz{};
        bool operator<(const Key& o) const
        {
            if (channel != o.channel) {
                return channel < o.channel;
            }
            return freq_hz < o.freq_hz;
        }
    };

    std::map<Key, std::vector<cal::InverseLutCandidate>> by_cf;
    for (const auto& e : direct) {
        cal::InverseLutCandidate c;
        c.att_code = e.att_code;
        c.phase_code = e.phase_code;
        c.a_meas_db = e.atten_meas_db;
        c.phi_meas_deg = e.phase_unwrapped_deg;
        c.valid = e.valid;
        by_cf[{e.channel, e.freq_hz}].push_back(c);
    }

    std::vector<double> target_att;
    for (const auto& row : att.rows) {
        if (row.enabled) {
            target_att.push_back(row.att_cmd_db);
        }
    }
    std::vector<double> target_ph;
    for (int ph = config.dut.phase_codes.first; ph <= config.dut.phase_codes.last; ++ph) {
        target_ph.push_back(static_cast<double>(ph) * config.dut.phase_codes.lsb_deg);
    }

    for (const auto& [key, candidates] : by_cf) {
        for (const double ta : target_att) {
            for (const double tp : target_ph) {
                const auto sel = cal::select_inverse_codes(ta, tp, candidates);
                InverseLutEntry e;
                e.channel = key.channel;
                e.freq_hz = key.freq_hz;
                e.target_atten_db = ta;
                e.target_phase_deg = tp;
                if (sel.found) {
                    e.selected_att_code = sel.selected_att_code;
                    e.selected_phase_code = sel.selected_phase_code;
                    e.measured_atten_db = sel.measured_atten_db;
                    e.measured_phase_deg = sel.measured_phase_deg;
                    e.atten_residual_db = sel.atten_residual_db;
                    e.phase_residual_deg = sel.phase_residual_deg;
                    e.valid = sel.valid;
                } else {
                    e.valid = false;
                }
                out.push_back(e);
            }
        }
    }
    return true;
}

std::size_t countValidDirect(std::span<const cal::DirectLutEntry> rows)
{
    std::size_t n = 0;
    for (const auto& e : rows) {
        if (e.valid) {
            ++n;
        }
    }
    return n;
}

std::size_t countValidInverse(std::span<const InverseLutEntry> rows)
{
    std::size_t n = 0;
    for (const auto& e : rows) {
        if (e.valid) {
            ++n;
        }
    }
    return n;
}

}  // namespace afar::report
