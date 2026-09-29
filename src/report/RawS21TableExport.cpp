#include "RawS21TableExport.h"

#include "RunEventLog.h"

#include <complex>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <tuple>
#include <vector>

namespace afar::report {
namespace {

using SlotKey = std::tuple<int, int, int>;

std::map<SlotKey, std::string> loadSlotTimestamps(const std::filesystem::path& run_events_path)
{
    std::map<SlotKey, std::string> out;
    std::vector<RunEvent> events;
    std::string diag;
    if (!RunEventLog::load(run_events_path, events, diag)) {
        return out;
    }
    for (const auto& ev : events) {
        if (ev.event_code != "STATE_OK") {
            continue;
        }
        if (!ev.channel || !ev.att_code || !ev.phase_code) {
            continue;
        }
        out[{*ev.channel, *ev.att_code, *ev.phase_code}] = ev.timestamp_utc;
    }
    return out;
}

std::string formatDouble(double v)
{
    std::ostringstream oss;
    oss << std::setprecision(17) << v;
    return oss.str();
}

std::string formatFloat(float v)
{
    std::ostringstream oss;
    oss << std::setprecision(9) << v;
    return oss.str();
}

bool csvEscapeNeeded(std::string_view s)
{
    return s.find_first_of(",\"\r\n") != std::string_view::npos;
}

std::string csvField(std::string_view s)
{
    if (!csvEscapeNeeded(s)) {
        return std::string(s);
    }
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}  // namespace

bool exportRawS21Csv(const std::filesystem::path& path,
                     const RawS21Store& store,
                     const std::string& run_id,
                     const std::filesystem::path& run_events_path,
                     std::string& diagnostics)
{
    diagnostics.clear();
    if (!store.isOpen()) {
        diagnostics = "RawS21Store is not open";
        return false;
    }

    const auto timestamps = loadSlotTimestamps(run_events_path);

    std::ostringstream csv;
    csv << "run_id,timestamp_utc,channel,att_code,phase_code,freq_hz,"
           "s21_re,s21_im,temp_c,attempt,overload,valid\n";

    for (const auto ch : store.channels()) {
        for (const auto att : store.attCodes()) {
            for (const auto ph : store.phaseCodes()) {
                if (!store.isCompleted(ch, att, ph)) {
                    continue;
                }
                RawS21StateRecord rec;
                if (!store.readState(ch, att, ph, rec, diagnostics)) {
                    return false;
                }
                std::string ts;
                const auto it = timestamps.find(
                    {static_cast<int>(ch), static_cast<int>(att), static_cast<int>(ph)});
                if (it != timestamps.end()) {
                    ts = it->second;
                }
                const auto& freqs = store.frequencyHz();
                for (std::size_t fi = 0; fi < freqs.size(); ++fi) {
                    const auto z =
                        (fi < rec.s21.size()) ? rec.s21[fi] : std::complex<double>{};
                    const bool point_valid =
                        (fi < rec.valid.size()) && (rec.valid[fi] != 0) && !rec.overload;
                    csv << csvField(run_id) << ',' << csvField(ts) << ','
                        << static_cast<unsigned>(ch) << ',' << att << ','
                        << static_cast<unsigned>(ph) << ',' << freqs[fi] << ','
                        << formatDouble(z.real()) << ',' << formatDouble(z.imag()) << ','
                        << formatFloat(rec.temperature_c) << ',' << rec.attempt << ','
                        << (rec.overload ? 1 : 0) << ',' << (point_valid ? 1 : 0) << '\n';
                }
            }
        }
    }

    const auto body = csv.str();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            diagnostics = "cannot open for write: " + path.string();
            return false;
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) {
            diagnostics = "write failed: " + path.string();
            return false;
        }
    }

    // Успех — после повторного открытия (заголовок на месте).
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot reopen: " + path.string();
        return false;
    }
    std::string header;
    if (!std::getline(in, header)) {
        diagnostics = "empty raw-s21.csv after write";
        return false;
    }
    if (!header.empty() && header.back() == '\r') {
        header.pop_back();
    }
    constexpr std::string_view kExpected =
        "run_id,timestamp_utc,channel,att_code,phase_code,freq_hz,"
        "s21_re,s21_im,temp_c,attempt,overload,valid";
    if (header != kExpected) {
        diagnostics = "unexpected raw-s21.csv header";
        return false;
    }
    return true;
}

}  // namespace afar::report
