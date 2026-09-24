#include "RunConfig.h"

#include "RunId.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <sstream>

namespace afar {
namespace {

using json = nlohmann::json;

constexpr std::uint64_t kFreqMinHz = 100'000ULL;
constexpr std::uint64_t kFreqMaxHz = 20'000'000'000ULL;
constexpr int kPointsMin = 2;
constexpr int kPointsMax = 500'001;
constexpr int kIfbwMinHz = 1;
constexpr int kIfbwMaxHz = 1'000'000;
constexpr double kPowerMinDbm = -60.0;
constexpr double kPowerMaxDbm = 10.0;

std::string readUtf8File(const std::filesystem::path& path, std::string& diagnostics)
{
    diagnostics.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot open file: " + path.string();
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // UTF-8 BOM
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

bool requireObjectKeys(const json& obj,
                       const std::set<std::string>& allowed,
                       const std::set<std::string>& required,
                       std::string_view path,
                       std::string& diagnostics)
{
    if (!obj.is_object()) {
        diagnostics = std::string(path) + ": expected object";
        return false;
    }
    for (const auto& [key, _] : obj.items()) {
        if (!allowed.contains(key)) {
            diagnostics = std::string(path) + ": unknown field '" + key + "' (additionalProperties)";
            return false;
        }
    }
    for (const auto& key : required) {
        if (!obj.contains(key)) {
            diagnostics = std::string(path) + ": missing required field '" + key + "'";
            return false;
        }
    }
    return true;
}

bool requireString(const json& v, std::string_view path, std::string& out, std::string& diagnostics)
{
    if (!v.is_string()) {
        diagnostics = std::string(path) + ": expected string";
        return false;
    }
    out = v.get<std::string>();
    if (out.empty()) {
        diagnostics = std::string(path) + ": must be non-empty";
        return false;
    }
    return true;
}

bool requireInt(const json& v, std::string_view path, std::int64_t& out, std::string& diagnostics)
{
    if (!v.is_number_integer()) {
        diagnostics = std::string(path) + ": expected integer";
        return false;
    }
    out = v.get<std::int64_t>();
    return true;
}

bool requireNumber(const json& v, std::string_view path, double& out, std::string& diagnostics)
{
    if (!v.is_number()) {
        diagnostics = std::string(path) + ": expected number";
        return false;
    }
    out = v.get<double>();
    return true;
}

bool requireBool(const json& v, std::string_view path, bool& out, std::string& diagnostics)
{
    if (!v.is_boolean()) {
        diagnostics = std::string(path) + ": expected boolean";
        return false;
    }
    out = v.get<bool>();
    return true;
}

bool parseVna(const json& j, VnaConfig& out, std::string& diagnostics)
{
    static const std::set<std::string> allowed = {
        "model", "host", "port", "s_parameter", "f_start_hz", "f_stop_hz",
        "points", "ifbw_hz", "power_dbm", "averages"};
    static const std::set<std::string> required = allowed;
    if (!requireObjectKeys(j, allowed, required, "vna", diagnostics)) {
        return false;
    }

    if (!requireString(j.at("model"), "vna.model", out.model, diagnostics)) {
        return false;
    }
    if (!requireString(j.at("host"), "vna.host", out.host, diagnostics)) {
        return false;
    }

    std::int64_t port = 0;
    if (!requireInt(j.at("port"), "vna.port", port, diagnostics)) {
        return false;
    }
    if (port < 1 || port > 65535) {
        diagnostics = "vna.port: out of range 1..65535";
        return false;
    }
    out.port = static_cast<int>(port);

    if (!requireString(j.at("s_parameter"), "vna.s_parameter", out.s_parameter, diagnostics)) {
        return false;
    }
    if (out.s_parameter != "S21") {
        diagnostics = "vna.s_parameter: must be 'S21' for stage 1";
        return false;
    }

    std::int64_t f_start = 0;
    std::int64_t f_stop = 0;
    if (!requireInt(j.at("f_start_hz"), "vna.f_start_hz", f_start, diagnostics)) {
        return false;
    }
    if (!requireInt(j.at("f_stop_hz"), "vna.f_stop_hz", f_stop, diagnostics)) {
        return false;
    }
    if (f_start < static_cast<std::int64_t>(kFreqMinHz)
        || f_start > static_cast<std::int64_t>(kFreqMaxHz)) {
        diagnostics = "vna.f_start_hz: out of instrument range 100000..20000000000 Hz";
        return false;
    }
    if (f_stop < static_cast<std::int64_t>(kFreqMinHz)
        || f_stop > static_cast<std::int64_t>(kFreqMaxHz)) {
        diagnostics = "vna.f_stop_hz: out of instrument range 100000..20000000000 Hz";
        return false;
    }
    if (!(f_start < f_stop)) {
        diagnostics = "vna.f_start_hz: must be strictly less than vna.f_stop_hz";
        return false;
    }
    out.f_start_hz = static_cast<std::uint64_t>(f_start);
    out.f_stop_hz = static_cast<std::uint64_t>(f_stop);

    std::int64_t points = 0;
    if (!requireInt(j.at("points"), "vna.points", points, diagnostics)) {
        return false;
    }
    if (points < kPointsMin || points > kPointsMax) {
        diagnostics = "vna.points: out of instrument range 2..500001";
        return false;
    }
    out.points = static_cast<int>(points);

    std::int64_t ifbw = 0;
    if (!requireInt(j.at("ifbw_hz"), "vna.ifbw_hz", ifbw, diagnostics)) {
        return false;
    }
    if (ifbw < kIfbwMinHz || ifbw > kIfbwMaxHz) {
        diagnostics = "vna.ifbw_hz: out of instrument range 1..1000000 Hz";
        return false;
    }
    out.ifbw_hz = static_cast<int>(ifbw);

    if (!requireNumber(j.at("power_dbm"), "vna.power_dbm", out.power_dbm, diagnostics)) {
        return false;
    }
    if (out.power_dbm < kPowerMinDbm || out.power_dbm > kPowerMaxDbm) {
        diagnostics = "vna.power_dbm: out of instrument range -60..+10 dBm";
        return false;
    }

    std::int64_t averages = 0;
    if (!requireInt(j.at("averages"), "vna.averages", averages, diagnostics)) {
        return false;
    }
    if (averages < 1 || averages > 999) {
        diagnostics = "vna.averages: out of instrument range 1..999";
        return false;
    }
    out.averages = static_cast<int>(averages);
    return true;
}

bool parseController(const json& j, ControllerConfig& out, std::string& diagnostics)
{
    static const std::set<std::string> allowed = {"driver", "endpoint"};
    if (!requireObjectKeys(j, allowed, allowed, "controller", diagnostics)) {
        return false;
    }
    return requireString(j.at("driver"), "controller.driver", out.driver, diagnostics)
        && requireString(j.at("endpoint"), "controller.endpoint", out.endpoint, diagnostics);
}

bool parseDut(const json& j, DutConfig& out, std::string& diagnostics)
{
    static const std::set<std::string> allowed = {
        "serial", "channels", "phase_codes", "attenuator_codes_file", "reference"};
    if (!requireObjectKeys(j, allowed, allowed, "dut", diagnostics)) {
        return false;
    }
    if (!requireString(j.at("serial"), "dut.serial", out.serial, diagnostics)) {
        return false;
    }

    {
        const json& ch = j.at("channels");
        static const std::set<std::string> ch_allowed = {"first", "last"};
        if (!requireObjectKeys(ch, ch_allowed, ch_allowed, "dut.channels", diagnostics)) {
            return false;
        }
        std::int64_t first = 0;
        std::int64_t last = 0;
        if (!requireInt(ch.at("first"), "dut.channels.first", first, diagnostics)) {
            return false;
        }
        if (!requireInt(ch.at("last"), "dut.channels.last", last, diagnostics)) {
            return false;
        }
        if (first < 1 || first > 16 || last < 1 || last > 16) {
            diagnostics = "dut.channels: channels must be in range 1..16";
            return false;
        }
        if (last < first) {
            diagnostics = "dut.channels: last must be >= first";
            return false;
        }
        out.channels.first = static_cast<int>(first);
        out.channels.last = static_cast<int>(last);
    }

    {
        const json& ph = j.at("phase_codes");
        static const std::set<std::string> ph_allowed = {"first", "last", "lsb_deg"};
        if (!requireObjectKeys(ph, ph_allowed, ph_allowed, "dut.phase_codes", diagnostics)) {
            return false;
        }
        std::int64_t first = 0;
        std::int64_t last = 0;
        if (!requireInt(ph.at("first"), "dut.phase_codes.first", first, diagnostics)) {
            return false;
        }
        if (!requireInt(ph.at("last"), "dut.phase_codes.last", last, diagnostics)) {
            return false;
        }
        if (first < 0 || first > 63 || last < 0 || last > 63) {
            diagnostics = "dut.phase_codes: codes must be in range 0..63";
            return false;
        }
        if (last < first) {
            diagnostics = "dut.phase_codes: last must be >= first";
            return false;
        }
        if (!requireNumber(ph.at("lsb_deg"), "dut.phase_codes.lsb_deg", out.phase_codes.lsb_deg,
                           diagnostics)) {
            return false;
        }
        if (!(out.phase_codes.lsb_deg > 0.0)) {
            diagnostics = "dut.phase_codes.lsb_deg: must be > 0";
            return false;
        }
        out.phase_codes.first = static_cast<int>(first);
        out.phase_codes.last = static_cast<int>(last);
    }

    if (!requireString(j.at("attenuator_codes_file"),
                       "dut.attenuator_codes_file",
                       out.attenuator_codes_file,
                       diagnostics)) {
        return false;
    }
    for (char c : out.attenuator_codes_file) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '.' || c == '_' || c == '-';
        if (!ok) {
            diagnostics = "dut.attenuator_codes_file: invalid characters (path separators forbidden)";
            return false;
        }
    }

    {
        const json& ref = j.at("reference");
        static const std::set<std::string> ref_allowed = {"att_code", "phase_code"};
        if (!requireObjectKeys(ref, ref_allowed, ref_allowed, "dut.reference", diagnostics)) {
            return false;
        }
        std::int64_t att = 0;
        std::int64_t phase = 0;
        if (!requireInt(ref.at("att_code"), "dut.reference.att_code", att, diagnostics)) {
            return false;
        }
        if (!requireInt(ref.at("phase_code"), "dut.reference.phase_code", phase, diagnostics)) {
            return false;
        }
        if (att < 0 || att > 63 || phase < 0 || phase > 63) {
            diagnostics = "dut.reference: att_code and phase_code must be in range 0..63";
            return false;
        }
        out.reference.att_code = static_cast<int>(att);
        out.reference.phase_code = static_cast<int>(phase);
    }
    return true;
}

bool parseTiming(const json& j, TimingConfig& out, std::string& diagnostics)
{
    static const std::set<std::string> allowed = {"settle_ms", "reference_after_phase_row"};
    if (!requireObjectKeys(j, allowed, allowed, "timing", diagnostics)) {
        return false;
    }
    std::int64_t settle = 0;
    if (!requireInt(j.at("settle_ms"), "timing.settle_ms", settle, diagnostics)) {
        return false;
    }
    if (settle < 0) {
        diagnostics = "timing.settle_ms: must be >= 0";
        return false;
    }
    out.settle_ms = static_cast<int>(settle);
    return requireBool(j.at("reference_after_phase_row"),
                       "timing.reference_after_phase_row",
                       out.reference_after_phase_row,
                       diagnostics);
}

bool parseLimits(const json& j, LimitsConfig& out, std::string& diagnostics)
{
    static const std::set<std::string> allowed = {"max_drift_phase_deg", "max_phase_residual_deg"};
    if (!requireObjectKeys(j, allowed, allowed, "limits", diagnostics)) {
        return false;
    }
    if (!requireNumber(j.at("max_drift_phase_deg"),
                       "limits.max_drift_phase_deg",
                       out.max_drift_phase_deg,
                       diagnostics)) {
        return false;
    }
    if (out.max_drift_phase_deg < 0.0) {
        diagnostics = "limits.max_drift_phase_deg: must be >= 0";
        return false;
    }
    if (!requireNumber(j.at("max_phase_residual_deg"),
                       "limits.max_phase_residual_deg",
                       out.max_phase_residual_deg,
                       diagnostics)) {
        return false;
    }
    if (out.max_phase_residual_deg < 0.0) {
        diagnostics = "limits.max_phase_residual_deg: must be >= 0";
        return false;
    }
    return true;
}

}  // namespace

bool RunConfig::parse(std::string_view utf8_json, RunConfig& out, std::string& diagnostics)
{
    diagnostics.clear();
    json root;
    try {
        root = json::parse(utf8_json);
    } catch (const json::parse_error& e) {
        diagnostics = std::string("JSON parse error: ") + e.what();
        return false;
    }

    static const std::set<std::string> root_allowed = {
        "schema", "run_id", "vna", "controller", "dut", "timing", "limits"};
    if (!requireObjectKeys(root, root_allowed, root_allowed, "root", diagnostics)) {
        return false;
    }

    RunConfig cfg;
    if (!requireString(root.at("schema"), "schema", cfg.schema, diagnostics)) {
        return false;
    }
    if (cfg.schema != kSchemaName) {
        diagnostics = "schema: expected '" + std::string(kSchemaName) + "', got '" + cfg.schema + "'";
        return false;
    }

    if (!requireString(root.at("run_id"), "run_id", cfg.run_id, diagnostics)) {
        return false;
    }
    if (!RunId::isValidUserId(cfg.run_id)) {
        diagnostics =
            "run_id: invalid (allowed: Latin letters, digits, hyphen, underscore; "
            "must start with letter or digit)";
        return false;
    }

    if (!parseVna(root.at("vna"), cfg.vna, diagnostics)) {
        return false;
    }
    if (!parseController(root.at("controller"), cfg.controller, diagnostics)) {
        return false;
    }
    if (!parseDut(root.at("dut"), cfg.dut, diagnostics)) {
        return false;
    }
    if (!parseTiming(root.at("timing"), cfg.timing, diagnostics)) {
        return false;
    }
    if (!parseLimits(root.at("limits"), cfg.limits, diagnostics)) {
        return false;
    }

    out = std::move(cfg);
    return true;
}

bool RunConfig::loadFromFile(const std::filesystem::path& path,
                             RunConfig& out,
                             std::string& diagnostics)
{
    const std::string text = readUtf8File(path, diagnostics);
    if (text.empty() && !diagnostics.empty()) {
        return false;
    }
    return parse(text, out, diagnostics);
}

}  // namespace afar
