#include "ParquetExport.h"

#include "InverseLut.h"
#include "Normalize.h"
#include "PhaseMath.h"
#include "QualityGates.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>

namespace afar::report {
namespace {

bool beginAtomicWrite(const std::filesystem::path& path,
                      std::filesystem::path& temporary,
                      std::ofstream& out,
                      std::string& diagnostics)
{
    temporary = path;
    temporary += ".tmp";
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    out.open(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
        diagnostics = "cannot open for write: " + temporary.string();
        return false;
    }
    return true;
}

bool beginAfarPqWrite(const std::filesystem::path& path,
                      std::filesystem::path& temporary,
                      std::ofstream& out,
                      std::string& diagnostics)
{
    if (!beginAtomicWrite(path, temporary, out, diagnostics)) {
        return false;
    }
    out.write(kAfarPqMagic, static_cast<std::streamsize>(sizeof(kAfarPqMagic)));
    return static_cast<bool>(out);
}

bool commitAtomicWrite(const std::filesystem::path& path,
                       const std::filesystem::path& temporary,
                       std::ofstream& out,
                       std::string& diagnostics)
{
    out.flush();
    if (!out) {
        diagnostics = "write failed: " + temporary.string();
        return false;
    }
    out.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        diagnostics = "cannot commit file: " + ec.message();
        return false;
    }
    return true;
}

bool openAfarPqRead(const std::filesystem::path& path,
                    std::ifstream& in,
                    std::string& diagnostics)
{
    in.open(path, std::ios::binary);
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
    return true;
}

std::vector<std::string_view> splitLine(std::string_view line, char separator)
{
    std::vector<std::string_view> cols;
    std::size_t start = 0;
    while (start <= line.size()) {
        const auto delimiter = line.find(separator, start);
        if (delimiter == std::string_view::npos) {
            cols.push_back(line.substr(start));
            break;
        }
        cols.push_back(line.substr(start, delimiter - start));
        start = delimiter + 1;
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
    oss.imbue(std::locale::classic());
    oss << std::setprecision(17) << v;
    return oss.str();
}

bool parseInverseColumns(std::span<const std::string_view> cols,
                         std::size_t offset,
                         InverseLutEntry& entry)
{
    return cols.size() >= offset + 11
        && parseNum(cols[offset], entry.channel)
        && parseNum(cols[offset + 1], entry.freq_hz)
        && parseNum(cols[offset + 2], entry.target_atten_db)
        && parseNum(cols[offset + 3], entry.target_phase_deg)
        && parseNum(cols[offset + 4], entry.selected_att_code)
        && parseNum(cols[offset + 5], entry.selected_phase_code)
        && parseNum(cols[offset + 6], entry.measured_atten_db)
        && parseNum(cols[offset + 7], entry.measured_phase_deg)
        && parseNum(cols[offset + 8], entry.atten_residual_db)
        && parseNum(cols[offset + 9], entry.phase_residual_deg)
        && parseBool(cols[offset + 10], entry.valid);
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

// Exact 2-D nearest-neighbour index for the inverse LUT cost. Phase is copied at
// -360/0/+360 degrees, so Euclidean distance is identical to wrap180().
class InverseCandidateIndex {
public:
    explicit InverseCandidateIndex(const std::vector<cal::InverseLutCandidate>& candidates)
        : candidates_(candidates)
    {
        const bool any_valid = std::any_of(candidates.begin(), candidates.end(), [](const auto& c) {
            return c.valid;
        });
        points_.reserve(candidates.size() * 3);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            if ((any_valid && !candidate.valid) || !std::isfinite(candidate.a_meas_db)
                || !std::isfinite(candidate.phi_meas_deg)) {
                continue;
            }
            double phase = std::fmod(candidate.phi_meas_deg, 360.0);
            if (phase < 0.0) {
                phase += 360.0;
            }
            for (const double shift : {-360.0, 0.0, 360.0}) {
                points_.push_back({candidate.a_meas_db, phase + shift, i});
            }
        }
        nodes_.reserve(points_.size());
        root_ = build(0, points_.size(), false);
    }

    cal::InverseLutResult select(double target_atten_db, double target_phase_deg) const
    {
        cal::InverseLutResult result;
        if (root_ < 0 || !std::isfinite(target_atten_db) || !std::isfinite(target_phase_deg)) {
            return result;
        }
        double phase = std::fmod(target_phase_deg, 360.0);
        if (phase < 0.0) {
            phase += 360.0;
        }
        double best_cost = std::numeric_limits<double>::infinity();
        std::size_t best_index = candidates_.size();
        nearest(root_, target_atten_db, phase, best_cost, best_index);
        if (best_index == candidates_.size()) {
            return result;
        }
        const auto& chosen = candidates_[best_index];
        result.selected_att_code = chosen.att_code;
        result.selected_phase_code = chosen.phase_code;
        result.measured_atten_db = chosen.a_meas_db;
        result.measured_phase_deg = chosen.phi_meas_deg;
        result.atten_residual_db = chosen.a_meas_db - target_atten_db;
        result.phase_residual_deg = cal::wrap180(chosen.phi_meas_deg - target_phase_deg);
        result.cost_j = best_cost;
        result.valid = chosen.valid;
        result.found = true;
        return result;
    }

private:
    struct Point {
        double attenuation{};
        double phase{};
        std::size_t candidate_index{};
    };
    struct Node {
        Point point;
        int left{-1};
        int right{-1};
        bool split_phase{false};
    };

    int build(std::size_t begin, std::size_t end, bool split_phase)
    {
        if (begin == end) {
            return -1;
        }
        const auto middle = begin + (end - begin) / 2;
        std::nth_element(points_.begin() + static_cast<std::ptrdiff_t>(begin),
                         points_.begin() + static_cast<std::ptrdiff_t>(middle),
                         points_.begin() + static_cast<std::ptrdiff_t>(end),
                         [split_phase](const Point& a, const Point& b) {
                             const double av = split_phase ? a.phase : a.attenuation;
                             const double bv = split_phase ? b.phase : b.attenuation;
                             if (av != bv) {
                                 return av < bv;
                             }
                             return a.candidate_index < b.candidate_index;
                         });
        const int index = static_cast<int>(nodes_.size());
        nodes_.push_back({points_[middle], -1, -1, split_phase});
        const int left = build(begin, middle, !split_phase);
        const int right = build(middle + 1, end, !split_phase);
        nodes_[static_cast<std::size_t>(index)].left = left;
        nodes_[static_cast<std::size_t>(index)].right = right;
        return index;
    }

    void nearest(int node_index,
                 double target_attenuation,
                 double target_phase,
                 double& best_cost,
                 std::size_t& best_index) const
    {
        if (node_index < 0) {
            return;
        }
        const auto& node = nodes_[static_cast<std::size_t>(node_index)];
        const double da = node.point.attenuation - target_attenuation;
        const double dp = node.point.phase - target_phase;
        const double cost = da * da + dp * dp;
        if (cost < best_cost || (cost == best_cost && node.point.candidate_index < best_index)) {
            best_cost = cost;
            best_index = node.point.candidate_index;
        }

        const double delta = node.split_phase ? dp : da;
        const int near_child = delta > 0.0 ? node.left : node.right;
        const int far_child = delta > 0.0 ? node.right : node.left;
        nearest(near_child, target_attenuation, target_phase, best_cost, best_index);
        if (delta * delta <= best_cost) {
            nearest(far_child, target_attenuation, target_phase, best_cost, best_index);
        }
    }

    const std::vector<cal::InverseLutCandidate>& candidates_;
    std::vector<Point> points_;
    std::vector<Node> nodes_;
    int root_{-1};
};

}  // namespace

bool exportDirectLut(const std::filesystem::path& path,
                     std::span<const cal::DirectLutEntry> rows,
                     std::string& diagnostics)
{
    std::filesystem::path temporary;
    std::ofstream out;
    if (!beginAfarPqWrite(path, temporary, out, diagnostics)) {
        return false;
    }
    out << "channel\tfreq_hz\tatt_code\tphase_code\ts21_re\ts21_im\tmag_db\t"
           "phase_unwrapped_deg\tatten_meas_db\tphase_error_deg\tdrift_phase_deg\t"
           "repeatability_db\trepeatability_deg\tvalid\n";
    for (const auto& e : rows) {
        out << static_cast<unsigned>(e.channel) << '\t' << e.freq_hz << '\t' << e.att_code
            << '\t' << static_cast<unsigned>(e.phase_code) << '\t' << formatDouble(e.s21_re)
            << '\t' << formatDouble(e.s21_im) << '\t' << formatDouble(e.mag_db) << '\t'
            << formatDouble(e.phase_unwrapped_deg) << '\t' << formatDouble(e.atten_meas_db)
            << '\t' << formatDouble(e.phase_error_deg) << '\t' << formatDouble(e.drift_phase_deg)
            << '\t' << formatDouble(e.repeatability_db) << '\t'
            << formatDouble(e.repeatability_deg) << '\t' << (e.valid ? 1 : 0) << '\n';
    }
    return commitAtomicWrite(path, temporary, out, diagnostics);
}

bool exportInverseLut(const std::filesystem::path& path,
                      std::span<const InverseLutEntry> rows,
                      std::string& diagnostics)
{
    std::filesystem::path temporary;
    std::ofstream out;
    if (!beginAfarPqWrite(path, temporary, out, diagnostics)) {
        return false;
    }
    out << "channel\tfreq_hz\ttarget_atten_db\ttarget_phase_deg\tselected_att_code\t"
           "selected_phase_code\tmeasured_atten_db\tmeasured_phase_deg\t"
           "atten_residual_db\tphase_residual_deg\tvalid\n";
    for (const auto& e : rows) {
        out << static_cast<unsigned>(e.channel) << '\t' << e.freq_hz << '\t'
            << formatDouble(e.target_atten_db) << '\t' << formatDouble(e.target_phase_deg)
            << '\t' << e.selected_att_code << '\t' << static_cast<unsigned>(e.selected_phase_code)
            << '\t' << formatDouble(e.measured_atten_db) << '\t'
            << formatDouble(e.measured_phase_deg) << '\t' << formatDouble(e.atten_residual_db)
            << '\t' << formatDouble(e.phase_residual_deg) << '\t' << (e.valid ? 1 : 0) << '\n';
    }
    return commitAtomicWrite(path, temporary, out, diagnostics);
}

bool exportInverseLutCsv(const std::filesystem::path& path,
                         std::string_view run_id,
                         std::span<const InverseLutEntry> rows,
                         std::string& diagnostics)
{
    if (run_id.empty() || run_id.find_first_of(",\r\n") != std::string_view::npos) {
        diagnostics = "run_id cannot be represented in calibration CSV";
        return false;
    }
    std::filesystem::path temporary;
    std::ofstream out;
    if (!beginAtomicWrite(path, temporary, out, diagnostics)) {
        return false;
    }
    out << "run_id,channel,freq_hz,target_atten_db,target_phase_deg,selected_att_code,"
           "selected_phase_code,measured_atten_db,measured_phase_deg,atten_residual_db,"
           "phase_residual_deg,valid\n";
    for (const auto& e : rows) {
        out << run_id << ',' << static_cast<unsigned>(e.channel) << ',' << e.freq_hz << ','
            << formatDouble(e.target_atten_db) << ',' << formatDouble(e.target_phase_deg) << ','
            << e.selected_att_code << ',' << static_cast<unsigned>(e.selected_phase_code) << ','
            << formatDouble(e.measured_atten_db) << ',' << formatDouble(e.measured_phase_deg)
            << ',' << formatDouble(e.atten_residual_db) << ','
            << formatDouble(e.phase_residual_deg) << ',' << (e.valid ? 1 : 0) << '\n';
    }
    return commitAtomicWrite(path, temporary, out, diagnostics);
}

bool readDirectLut(const std::filesystem::path& path,
                   std::vector<cal::DirectLutEntry>& out,
                   std::string& diagnostics)
{
    out.clear();
    std::ifstream in;
    if (!openAfarPqRead(path, in, diagnostics)) {
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) {
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
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto cols = splitLine(line, '\t');
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
    std::ifstream in;
    if (!openAfarPqRead(path, in, diagnostics)) {
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) {
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
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto cols = splitLine(line, '\t');
        if (cols.size() < 11) {
            diagnostics = "inverse lut row too short";
            return false;
        }
        InverseLutEntry e;
        if (!parseInverseColumns(cols, 0, e)) {
            diagnostics = "inverse lut parse error";
            return false;
        }
        out.push_back(e);
    }
    return true;
}

bool readInverseLutCsv(const std::filesystem::path& path,
                       std::string& run_id,
                       std::vector<InverseLutEntry>& out,
                       std::string& diagnostics)
{
    run_id.clear();
    out.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot open for read: " + path.string();
        return false;
    }
    std::string line;
    if (!std::getline(in, line)) {
        diagnostics = "empty inverse lut csv";
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    constexpr std::string_view expectedHeader =
        "run_id,channel,freq_hz,target_atten_db,target_phase_deg,selected_att_code,"
        "selected_phase_code,measured_atten_db,measured_phase_deg,atten_residual_db,"
        "phase_residual_deg,valid";
    if (line != expectedHeader) {
        diagnostics = "unexpected inverse lut csv header";
        return false;
    }
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto cols = splitLine(line, ',');
        InverseLutEntry entry;
        if (cols.size() != 12 || cols[0].empty() || !parseInverseColumns(cols, 1, entry)) {
            diagnostics = "inverse lut csv parse error";
            return false;
        }
        if (run_id.empty()) {
            run_id.assign(cols[0]);
        } else if (cols[0] != run_id) {
            diagnostics = "inverse lut csv contains multiple run_id values";
            return false;
        }
        out.push_back(entry);
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
                    in.repeatability_db = fi < rec.repeatability_db.size()
                        ? rec.repeatability_db[fi]
                        : std::numeric_limits<double>::quiet_NaN();
                    in.repeatability_deg = fi < rec.repeatability_deg.size()
                        ? rec.repeatability_deg[fi]
                        : std::numeric_limits<double>::quiet_NaN();
                    in.sample_valid = (fi < rec.valid.size()) && (rec.valid[fi] != 0)
                        && (fi < norm.size()) && norm[fi].valid && !rec.overload;
                    qc::QualityInputs repeatability_qc;
                    repeatability_qc.repeatability_db = in.repeatability_db;
                    repeatability_qc.repeatability_deg = in.repeatability_deg;
                    if (!qc::evaluate_valid(repeatability_qc)) {
                        in.sample_valid = false;
                    }
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
    out.reserve(by_cf.size() * target_att.size() * target_ph.size());

    for (const auto& [key, candidates] : by_cf) {
        const InverseCandidateIndex index(candidates);
        for (const double ta : target_att) {
            for (const double tp : target_ph) {
                const auto sel = index.select(ta, tp);
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
                    // FR-17 / AT-10: остаток фазы выше порога → valid=false в отчёте.
                    qc::QualityThresholds th;
                    th.max_phase_residual_deg = config.limits.max_phase_residual_deg;
                    e.valid = sel.valid
                        && qc::phase_residual_within_limit(sel.phase_residual_deg, th);
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
