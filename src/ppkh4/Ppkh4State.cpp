#include "Ppkh4State.h"

#include <nlohmann/json.hpp>

namespace afar::ppkh4 {

namespace {

using nlohmann::json;

const char* mode_name(SweepMode mode)
{
    switch (mode) {
    case SweepMode::Atten: return "atten";
    case SweepMode::Grid: return "grid";
    case SweepMode::Together: return "together";
    case SweepMode::Phase: break;
    }
    return "phase";
}

bool parse_mode(const std::string& text, SweepMode& mode)
{
    for (const SweepMode m : {SweepMode::Phase, SweepMode::Atten, SweepMode::Grid, SweepMode::Together}) {
        if (text == mode_name(m)) {
            mode = m;
            return true;
        }
    }
    return false;
}

json field_json(const FieldFormat& f)
{
    return {{"offset", f.offset}, {"width", f.width}, {"lsb", f.lsb}, {"invert", f.invert}, {"reverse", f.reverse}};
}

FieldFormat field_from(const json& j)
{
    FieldFormat f;
    f.offset = j.at("offset").get<int>();
    f.width = j.at("width").get<int>();
    f.lsb = j.at("lsb").get<double>();
    f.invert = j.at("invert").get<bool>();
    f.reverse = j.at("reverse").get<bool>();
    return f;
}

json range_json(const SweepRange& r)
{
    return {{"from", r.from}, {"to", r.to}, {"step", r.step}};
}

SweepRange range_from(const json& j)
{
    return {j.at("from").get<int>(), j.at("to").get<int>(), j.at("step").get<int>()};
}

}  // namespace

std::string to_json_text(const BenchState& state)
{
    const ChannelState& c = state.channel;
    const json j = {
        {"format", "afar-bench-state"},
        {"version", kStateFormatVersion},
        {"name", state.name},
        {"saved_at", state.saved_at},
        {"s2vna_state_file", state.s2vna_state_file},
        {"single_channel",
         {{"frame",
           {{"total_bits", c.format.total_bits},
            {"fixed_hex", c.format.fixed_hex},
            {"phase", field_json(c.format.phase)},
            {"atten", field_json(c.format.atten)}}},
          {"lines",
           {{"lsb_first", c.lines.lsb_first},
            {"clk_idle", c.lines.clk_idle},
            {"le_active", c.lines.le_active},
            {"le_mode", c.lines.le_mode},
            {"half_us", c.lines.half_us},
            {"le_us", c.lines.le_us}}},
          {"phase_code", c.phase_code},
          {"atten_code", c.atten_code},
          {"sweep",
           {{"mode", mode_name(c.sweep_mode)},
            {"phase", range_json(c.sweep_phase)},
            {"atten", range_json(c.sweep_atten)},
            {"dwell_ms", c.sweep_dwell_ms}}}}},
    };
    return j.dump(2) + "\n";
}

bool from_json_text(const std::string& text, BenchState& state, std::string* error)
{
    auto fail = [error](const std::string& why) {
        if (error) {
            *error = why;
        }
        return false;
    };
    BenchState out;
    try {
        const json j = json::parse(text);
        if (j.value("format", std::string()) != "afar-bench-state") {
            return fail("это не файл состояния стенда");
        }
        const int version = j.at("version").get<int>();
        if (version != kStateFormatVersion) {
            return fail("неизвестная версия файла: " + std::to_string(version));
        }
        out.name = j.value("name", std::string());
        out.saved_at = j.value("saved_at", std::string());
        out.s2vna_state_file = j.value("s2vna_state_file", std::string());
        const json& sc = j.at("single_channel");
        const json& frame = sc.at("frame");
        out.channel.format.total_bits = frame.at("total_bits").get<int>();
        out.channel.format.fixed_hex = frame.at("fixed_hex").get<std::string>();
        out.channel.format.phase = field_from(frame.at("phase"));
        out.channel.format.atten = field_from(frame.at("atten"));
        const json& lines = sc.at("lines");
        out.channel.lines.lsb_first = lines.at("lsb_first").get<int>();
        out.channel.lines.clk_idle = lines.at("clk_idle").get<int>();
        out.channel.lines.le_active = lines.at("le_active").get<int>();
        out.channel.lines.le_mode = lines.at("le_mode").get<int>();
        out.channel.lines.half_us = lines.at("half_us").get<int>();
        out.channel.lines.le_us = lines.at("le_us").get<int>();
        out.channel.phase_code = sc.at("phase_code").get<int>();
        out.channel.atten_code = sc.at("atten_code").get<int>();
        const json& sweep = sc.at("sweep");
        if (!parse_mode(sweep.at("mode").get<std::string>(), out.channel.sweep_mode)) {
            return fail("неизвестный режим перебора");
        }
        out.channel.sweep_phase = range_from(sweep.at("phase"));
        out.channel.sweep_atten = range_from(sweep.at("atten"));
        out.channel.sweep_dwell_ms = sweep.at("dwell_ms").get<int>();
    } catch (const std::exception& e) {
        return fail(std::string("файл не разобран: ") + e.what());
    }
    if (!format_valid(out.channel.format)) {
        return fail("недопустимая раскладка кадра");
    }
    if (out.s2vna_state_file.find_first_of("/\\\"\r\n") != std::string::npos) {
        return fail("имя файла S2VNA должно быть без каталога");
    }
    out.channel.phase_code = clamp_code(out.channel.phase_code, out.channel.format.phase);
    out.channel.atten_code = clamp_code(out.channel.atten_code, out.channel.format.atten);
    state = std::move(out);
    return true;
}

}  // namespace afar::ppkh4
