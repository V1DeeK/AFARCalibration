#include "C2220Vna.h"

#include "afar/C2220Limits.h"
#include "NumberText.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

bool is_no_error_response(const std::string& line)
{
    // Формат: 0,"No error" / 0, "No error" / 0,No error
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (i >= line.size() || line[i] != '0') {
        return false;
    }
    ++i;
    if (i < line.size() && line[i] != ',' && line[i] != ' ' && line[i] != '\t') {
        // число вроде 100, ...
        return false;
    }
    return true;
}

bool scpi_state_is_on(const std::string& reply)
{
    std::string s = reply;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
    }
    const std::string body = s.substr(i);
    if (body == "1" || body == "ON" || body == "on" || body == "On") {
        return true;
    }
    return false;
}

std::string trim_scpi_text(std::string value)
{
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.pop_back();
    }
    std::size_t first = 0;
    while (first < value.size() && (value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    value = value.substr(first);
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::vector<double> parse_ascii_doubles(const std::string& line)
{
    std::vector<double> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size()
               && (line[i] == ' ' || line[i] == '\t' || line[i] == ',' || line[i] == ';')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ',' && line[i] != ';' && line[i] != ' '
               && line[i] != '\t') {
            ++i;
        }
        const std::string token = line.substr(start, i - start);
        if (token.empty()) {
            continue;
        }
        double value = 0.0;
        if (!afar::parse_double(token, value)) {
            throw std::runtime_error("C2220Vna: cannot parse number '" + token + "'");
        }
        out.push_back(value);
    }
    return out;
}

std::string format_double(double v)
{
    std::ostringstream oss;
    oss.precision(15);
    oss << v;
    return oss.str();
}

const char* s_parameter_scpi(SParameter p)
{
    switch (p) {
    case SParameter::S11:
        return "S11";
    case SParameter::S21:
        return "S21";
    case SParameter::S12:
        return "S12";
    case SParameter::S22:
        return "S22";
    }
    return "S21";
}

double parse_single_double(const std::string& reply, const char* command)
{
    const auto values = parse_ascii_doubles(reply);
    if (values.size() != 1 || !std::isfinite(values.front())) {
        throw std::runtime_error(std::string("C2220Vna: unexpected ") + command
                                 + " reply: " + reply);
    }
    return values.front();
}

SParameter parse_s_parameter(const std::string& reply)
{
    if (reply.find("S11") != std::string::npos) {
        return SParameter::S11;
    }
    if (reply.find("S21") != std::string::npos) {
        return SParameter::S21;
    }
    if (reply.find("S12") != std::string::npos) {
        return SParameter::S12;
    }
    if (reply.find("S22") != std::string::npos) {
        return SParameter::S22;
    }
    throw std::runtime_error("C2220Vna: unknown CALC:PAR:DEF? reply: " + reply);
}

}  // namespace

C2220Vna::C2220Vna(IScpiTransport& transport)
    : transport_(transport)
{
}

C2220Vna::C2220Vna(IScpiTransport& transport, Profile profile)
    : transport_(transport)
    , profile_(std::move(profile))
{
    if (profile_.measure_retries < 0) {
        profile_.measure_retries = 0;
    }
    if (profile_.required_model.empty()) {
        profile_.required_model = "C2220";
    }
}

void C2220Vna::require_connected(const char* op) const
{
    if (!connected_) {
        throw std::runtime_error(std::string("C2220Vna: ") + op + " without connect");
    }
}

void C2220Vna::write_cmd(const std::string& cmd)
{
    transport_.write_line(cmd);
}

std::string C2220Vna::query(const std::string& cmd)
{
    transport_.write_line(cmd);
    return transport_.read_line();
}

void C2220Vna::reject_if_foreign_model(const std::string& idn) const
{
    if (idn.find(profile_.required_model) == std::string::npos) {
        throw std::runtime_error("C2220Vna: foreign VNA model in *IDN?: " + idn);
    }
}

void C2220Vna::check_direct_access()
{
    // Только запрос. Включение (ON/1) программа никогда не посылает.
    const std::string reply = query("SYST:REC:DIR:ACC?");
    if (scpi_state_is_on(reply) && !profile_.allow_direct_access) {
        throw std::runtime_error(
            "C2220Vna: direct receiver access is ON but profile.allow_direct_access is false");
    }
}

void C2220Vna::connect()
{
    if (connected_ && transport_.is_connected()) return;
    transport_.set_io_timeout_ms(profile_.connect_timeout_ms);
    transport_.connect();
    connected_ = true;
    configured_ = false;

    try {
        // На старте связи / серии — проверка прямого доступа (HW-VNA-05).
        transport_.set_io_timeout_ms(profile_.sweep_timeout_ms);
        check_direct_access();
    } catch (...) {
        connected_ = false;
        transport_.disconnect();
        throw;
    }
}

std::string C2220Vna::identify()
{
    require_connected("identify");
    const std::string idn = query("*IDN?");
    reject_if_foreign_model(idn);
    last_idn_ = parse_scpi_idn(idn);
    return last_idn_.raw;
}

void C2220Vna::configure(const SweepConfig& config)
{
    require_connected("configure");
    std::string diagnostics;
    if (!afar::c2220::validateSweep(config, diagnostics)) {
        throw std::invalid_argument("C2220Vna: " + diagnostics);
    }

    write_cmd("SENS:FREQ:STAR " + std::to_string(config.f_start_hz));
    write_cmd("SENS:FREQ:STOP " + std::to_string(config.f_stop_hz));
    write_cmd("SENS:SWE:POIN " + std::to_string(config.points));
    write_cmd("SENS:BAND " + std::to_string(config.ifbw_hz));
    write_cmd("SOUR:POW " + format_double(config.power_dbm));
    write_cmd("SENS:AVER:COUN " + std::to_string(config.averages));
    write_cmd(std::string("SENS:AVER ") + (config.averages > 1 ? "ON" : "OFF"));
    write_cmd(std::string("TRIG:AVER ") + (config.averages > 1 ? "ON" : "OFF"));
    // S2VNA в INT может начать следующий непрерывный свип между DEF и чтением SDAT.
    // BUS + явный выбор графика гарантируют, что *OPC? относится к нужному S-параметру.
    write_cmd("TRIG:SOUR BUS");
    write_cmd(std::string("CALC:PAR:DEF ") + s_parameter_scpi(config.s_parameter));
    write_cmd("CALC:PAR:SEL");

    config_ = config;
    configured_ = true;
}

C2220Vna::TriggerState C2220Vna::enable_native_continuous()
{
    require_connected("enable_native_continuous");
    const std::string source = trim_scpi_text(query("TRIG:SOUR?"));
    const std::string continuous = trim_scpi_text(query("INIT:CONT?"));
    if (source != "INT" && source != "EXT" && source != "BUS" && source != "MAN") {
        throw std::runtime_error("C2220Vna: unknown trigger source: " + source);
    }
    if (continuous != "0" && continuous != "1"
        && continuous != "ON" && continuous != "OFF") {
        throw std::runtime_error("C2220Vna: unknown continuous mode: " + continuous);
    }
    const TriggerState previous{source, continuous == "1" || continuous == "ON"};
    try {
        if (!previous.continuous) write_cmd("INIT:CONT ON");
        if (previous.source != "INT") write_cmd("TRIG:SOUR INT");
    } catch (...) {
        try {
            write_cmd("TRIG:SOUR " + previous.source);
            write_cmd(std::string("INIT:CONT ") + (previous.continuous ? "ON" : "OFF"));
        } catch (...) {
        }
        throw;
    }
    return previous;
}

void C2220Vna::restore_native_trigger(const TriggerState& previous)
{
    require_connected("restore_native_trigger");
    // Если настройки успел изменить оператор или другой сценарий, не затрём их.
    if (trim_scpi_text(query("TRIG:SOUR?")) != "INT"
        || !scpi_state_is_on(query("INIT:CONT?"))) {
        return;
    }
    if (previous.source != "INT") write_cmd("TRIG:SOUR " + previous.source);
    if (!previous.continuous) write_cmd("INIT:CONT OFF");
}

void C2220Vna::load_state(const std::string& path)
{
    require_connected("load_state");
    if (path.empty() || path.find_first_of("\"\r\n") != std::string::npos) {
        throw std::invalid_argument("C2220Vna: invalid state file path");
    }
    configured_ = false; // После попытки восстановления предыдущая SweepConfig уже ненадёжна.
    write_cmd("MMEM:LOAD:STAT \"" + path + "\"");
    if (trim_scpi_text(query("*OPC?")) != "1") {
        throw std::runtime_error("C2220Vna: state restore did not complete");
    }
    const auto errors = drain_errors();
    if (!errors.empty()) {
        throw std::runtime_error("C2220Vna: state restore failed: " + errors.front());
    }
}

void C2220Vna::save_state(const std::string& path)
{
    require_connected("save_state");
    if (path.empty() || path.find_first_of("\"\r\n") != std::string::npos) {
        throw std::invalid_argument("C2220Vna: invalid state file path");
    }
    const auto previous = trim_scpi_text(query("MMEM:STOR:STYP?"));
    if (previous != "STAT" && previous != "CST" && previous != "DST"
        && previous != "CDST" && previous != "CMST") {
        throw std::runtime_error("C2220Vna: unknown state save type: " + previous);
    }
    const bool changed = previous != "CDST";
    try {
        if (changed) write_cmd("MMEM:STOR:STYP CDST");
        write_cmd("MMEM:STOR:STAT \"" + path + "\"");
        if (trim_scpi_text(query("*OPC?")) != "1") {
            throw std::runtime_error("C2220Vna: state save did not complete");
        }
        const auto errors = drain_errors();
        if (!errors.empty()) {
            throw std::runtime_error("C2220Vna: state save failed: " + errors.front());
        }
    } catch (...) {
        if (changed) {
            try { write_cmd("MMEM:STOR:STYP " + previous); } catch (...) {}
        }
        throw;
    }
    if (changed) {
        write_cmd("MMEM:STOR:STYP " + previous);
        if (trim_scpi_text(query("MMEM:STOR:STYP?")) != previous) {
            throw std::runtime_error("C2220Vna: state save type was not restored");
        }
    }
}

SweepConfig C2220Vna::apply_native_sweep(const SweepConfig& config)
{
    require_connected("apply_native_sweep");
    std::string diagnostics;
    if (!afar::c2220::validateSweep(config, diagnostics)) {
        throw std::invalid_argument("C2220Vna: " + diagnostics);
    }
    write_cmd("SENS:FREQ:STAR " + std::to_string(config.f_start_hz));
    write_cmd("SENS:FREQ:STOP " + std::to_string(config.f_stop_hz));
    write_cmd("SENS:SWE:POIN " + std::to_string(config.points));
    write_cmd("SENS:BAND " + std::to_string(config.ifbw_hz));
    write_cmd("SOUR:POW " + format_double(config.power_dbm));
    write_cmd("SENS:AVER:COUN " + std::to_string(config.averages));
    write_cmd(std::string("SENS:AVER ") + (config.averages > 1 ? "ON" : "OFF"));
    write_cmd(std::string("TRIG:AVER ") + (config.averages > 1 ? "ON" : "OFF"));
    if (trim_scpi_text(query("*OPC?")) != "1") {
        throw std::runtime_error("C2220Vna: sweep update did not complete");
    }
    const auto errors = drain_errors();
    if (!errors.empty()) {
        throw std::runtime_error("C2220Vna: sweep update failed: " + errors.front());
    }
    return read_config();
}

C2220Vna::NativeOperatingState C2220Vna::native_operating_state()
{
    require_connected("native_operating_state");
    const auto parse = [](const std::string& value) {
        const auto text = trim_scpi_text(value);
        if (text == "1" || text == "ON") return true;
        if (text == "0" || text == "OFF") return false;
        throw std::runtime_error("C2220Vna: invalid RF/correction state: " + text);
    };
    return {parse(query("OUTP?")), parse(query("SENS:CORR:STAT?"))};
}

C2220Vna::NativeOperatingState C2220Vna::set_native_operating_state(
    bool rf_output, bool correction)
{
    require_connected("set_native_operating_state");
    const auto current = native_operating_state();
    if (!rf_output && current.rf_output) write_cmd("OUTP OFF");
    if (current.correction != correction) {
        write_cmd(std::string("SENS:CORR:STAT ") + (correction ? "ON" : "OFF"));
    }
    if (rf_output && !current.rf_output) write_cmd("OUTP ON");
    if (trim_scpi_text(query("*OPC?")) != "1") {
        throw std::runtime_error("C2220Vna: RF/correction update did not complete");
    }
    const auto errors = drain_errors();
    if (!errors.empty()) {
        throw std::runtime_error("C2220Vna: RF/correction update failed: " + errors.front());
    }
    const auto applied = native_operating_state();
    if (applied.rf_output != rf_output || applied.correction != correction) {
        throw std::runtime_error("C2220Vna: RF/correction readback mismatch");
    }
    return applied;
}

C2220Vna::NativeGraphState C2220Vna::native_graph_state()
{
    require_connected("native_graph_state");
    const int count = static_cast<int>(parse_single_double(query("CALC1:PAR:COUN?"),
                                                            "CALC1:PAR:COUN?"));
    if (count < 1 || count > 16) {
        throw std::runtime_error("C2220Vna: invalid native trace count");
    }
    NativeGraphState state;
    state.layout = static_cast<int>(parse_single_double(query("DISP:WIND1:SPL?"),
                                                         "DISP:WIND1:SPL?"));
    for (int trace = 1; trace <= std::min(count, 6); ++trace) {
        const std::string number = std::to_string(trace);
        state.parameters.push_back(trim_scpi_text(query("CALC1:PAR" + number + ":DEF?")));
        state.formats.push_back(trim_scpi_text(query("CALC1:TRAC" + number + ":FORM?")));
    }
    return state;
}

void C2220Vna::configure_native_graph(const std::vector<NativeTrace>& traces, bool separate)
{
    require_connected("configure_native_graph");
    if (traces.empty() || traces.size() > 6) {
        throw std::invalid_argument("C2220Vna: native graph needs 1..6 traces");
    }
    for (const auto& trace : traces) {
        if (trace.format != "MLOG" && trace.format != "MLIN" && trace.format != "PHAS"
            && trace.format != "GDEL" && trace.format != "SWR") {
            throw std::invalid_argument("C2220Vna: unsupported native graph format");
        }
        if (trace.format == "SWR" && trace.parameter != SParameter::S11
            && trace.parameter != SParameter::S22) {
            throw std::invalid_argument("C2220Vna: SWR requires S11 or S22");
        }
    }
    const int count = static_cast<int>(traces.size());
    const int layout = !separate || count == 1 ? 1
        : count == 2 ? 2 : count == 3 ? 4 : count == 4 ? 7 : 9;
    write_cmd("CALC1:PAR:COUN " + std::to_string(count));
    for (int i = 0; i < count; ++i) {
        const std::string number = std::to_string(i + 1);
        write_cmd("CALC1:PAR" + number + ":DEF " + s_parameter_scpi(traces[i].parameter));
        write_cmd("CALC1:TRAC" + number + ":FORM " + traces[i].format);
    }
    write_cmd("DISP:WIND1:SPL " + std::to_string(layout));
    if (trim_scpi_text(query("*OPC?")) != "1") {
        throw std::runtime_error("C2220Vna: native graph configuration did not complete");
    }
    const auto errors = drain_errors();
    if (!errors.empty()) {
        throw std::runtime_error("C2220Vna: native graph configuration failed: " + errors.front());
    }
    const auto applied = native_graph_state();
    if (applied.layout != layout || applied.parameters.size() != traces.size()) {
        throw std::runtime_error("C2220Vna: native graph layout was not applied");
    }
    for (int i = 0; i < count; ++i) {
        if (applied.parameters[i] != s_parameter_scpi(traces[i].parameter)
            || applied.formats[i] != traces[i].format) {
            throw std::runtime_error("C2220Vna: native graph trace was not applied");
        }
    }
}

void C2220Vna::restore_native_graph_state(const NativeGraphState& state)
{
    require_connected("restore_native_graph_state");
    if (state.parameters.empty() || state.parameters.size() > 6
        || state.parameters.size() != state.formats.size()
        || state.layout < 1 || state.layout > 9) {
        throw std::invalid_argument("C2220Vna: invalid saved graph state");
    }
    for (std::size_t i = 0; i < state.parameters.size(); ++i) {
        const auto& parameter = state.parameters[i];
        const auto& format = state.formats[i];
        if (parameter != "S11" && parameter != "S21" && parameter != "S12"
            && parameter != "S22") {
            throw std::invalid_argument("C2220Vna: invalid saved S-parameter");
        }
        if (format.empty() || format.size() > 8
            || !std::all_of(format.begin(), format.end(), [](unsigned char c) {
                return std::isupper(c) || std::isdigit(c);
            })) {
            throw std::invalid_argument("C2220Vna: invalid saved graph format");
        }
    }
    const auto current = native_graph_state();
    if (current.parameters.size() != state.parameters.size()) {
        write_cmd("CALC1:PAR:COUN " + std::to_string(state.parameters.size()));
    }
    for (std::size_t i = 0; i < state.parameters.size(); ++i) {
        const auto number = std::to_string(i + 1);
        if (i >= current.parameters.size() || current.parameters[i] != state.parameters[i]) {
            write_cmd("CALC1:PAR" + number + ":DEF " + state.parameters[i]);
        }
        if (i >= current.formats.size() || current.formats[i] != state.formats[i]) {
            write_cmd("CALC1:TRAC" + number + ":FORM " + state.formats[i]);
        }
    }
    if (current.layout != state.layout) {
        write_cmd("DISP:WIND1:SPL " + std::to_string(state.layout));
    }
    if (trim_scpi_text(query("*OPC?")) != "1") {
        throw std::runtime_error("C2220Vna: graph restoration did not complete");
    }
    const auto errors = drain_errors();
    if (!errors.empty()) {
        throw std::runtime_error("C2220Vna: graph restoration failed: " + errors.front());
    }
    const auto restored = native_graph_state();
    if (restored.parameters != state.parameters || restored.formats != state.formats
        || restored.layout != state.layout) {
        throw std::runtime_error("C2220Vna: graph restoration readback mismatch");
    }
}

void C2220Vna::auto_scale_native_graph()
{
    require_connected("auto_scale_native_graph");
    const int count = static_cast<int>(parse_single_double(query("CALC1:PAR:COUN?"),
                                                            "CALC1:PAR:COUN?"));
    if (count < 1 || count > 16) {
        throw std::runtime_error("C2220Vna: invalid native trace count");
    }
    for (int trace = 1; trace <= count; ++trace) {
        write_cmd("DISP:WIND1:TRAC" + std::to_string(trace) + ":Y:AUTO");
    }
    if (trim_scpi_text(query("*OPC?")) != "1") {
        throw std::runtime_error("C2220Vna: native auto scale did not complete");
    }
}

int C2220Vna::native_marker_count(int trace)
{
    require_connected("native_marker_count");
    if (trace < 1 || trace > 16) throw std::invalid_argument("C2220Vna: invalid trace");
    const std::string command = "CALC1:TRAC" + std::to_string(trace) + ":MARK:COUN?";
    const int count = static_cast<int>(parse_single_double(query(command), command.c_str()));
    if (count < 0 || count > 16) throw std::runtime_error("C2220Vna: invalid marker count");
    return count;
}

bool C2220Vna::native_reference_marker_active(int trace)
{
    require_connected("native_reference_marker_active");
    if (trace < 1 || trace > 16) throw std::invalid_argument("C2220Vna: invalid trace");
    const std::string command = "CALC1:TRAC" + std::to_string(trace) + ":MARK16:STAT?";
    return parse_single_double(query(command), command.c_str()) != 0.0;
}

void C2220Vna::set_native_marker_count(int trace, int count)
{
    require_connected("set_native_marker_count");
    if (trace < 1 || trace > 16 || count < 0 || count > 15) {
        throw std::invalid_argument("C2220Vna: invalid trace or marker count");
    }
    if (native_reference_marker_active(trace)) {
        throw std::runtime_error("C2220Vna: reference marker is active");
    }
    write_cmd("CALC1:TRAC" + std::to_string(trace) + ":MARK:COUN " + std::to_string(count));
    if (native_marker_count(trace) != count) {
        throw std::runtime_error("C2220Vna: marker count was not applied");
    }
}

std::vector<C2220Vna::NativeMarker> C2220Vna::native_markers(int trace)
{
    const int count = std::min(native_marker_count(trace)
                                   - static_cast<int>(native_reference_marker_active(trace)), 15);
    std::vector<NativeMarker> markers;
    markers.reserve(count);
    for (int marker = 1; marker <= count; ++marker) {
        const std::string prefix = "CALC1:TRAC" + std::to_string(trace)
            + ":MARK" + std::to_string(marker);
        NativeMarker value;
        value.frequency_hz = parse_single_double(query(prefix + ":X?"), "native marker X");
        const auto y = parse_ascii_doubles(query(prefix + ":Y?"));
        if (y.size() < 2) throw std::runtime_error("C2220Vna: invalid native marker Y");
        value.value = y[0];
        value.secondary = y[1];
        markers.push_back(value);
    }
    return markers;
}

std::vector<std::string> C2220Vna::native_trace_parameters()
{
    require_connected("native_trace_parameters");
    const int count = static_cast<int>(parse_single_double(query("CALC1:PAR:COUN?"),
                                                            "CALC1:PAR:COUN?"));
    if (count < 1 || count > 16) {
        throw std::runtime_error("C2220Vna: invalid native trace count");
    }
    std::vector<std::string> result;
    for (int i = 1; i <= std::min(count, 4); ++i) {
        result.push_back(trim_scpi_text(query("CALC1:PAR" + std::to_string(i) + ":DEF?")));
    }
    return result;
}

void C2220Vna::set_native_trace_parameter(int trace, SParameter parameter)
{
    require_connected("set_native_trace_parameter");
    if (trace < 1 || trace > 4
        || trace > static_cast<int>(parse_single_double(query("CALC1:PAR:COUN?"),
                                                         "CALC1:PAR:COUN?"))) {
        throw std::invalid_argument("C2220Vna: trace is not open");
    }
    const std::string prefix = "CALC1:PAR" + std::to_string(trace) + ":DEF";
    const std::string requested = s_parameter_scpi(parameter);
    write_cmd(prefix + " " + requested);
    if (trim_scpi_text(query(prefix + "?")) != requested) {
        throw std::runtime_error("C2220Vna: trace parameter was not applied");
    }
}

ComplexSweep C2220Vna::measure_once()
{
    if (config_.averages > 1) {
        write_cmd("SENS:AVER:CLE");
    }
    write_cmd("TRIG:SING");
    const std::string opc = query("*OPC?");
    if (opc.find('1') == std::string::npos) {
        throw std::runtime_error("C2220Vna: unexpected *OPC? reply: " + opc);
    }

    return read_trace_data(config_.s_parameter, config_.points);
}

ComplexSweep C2220Vna::read_trace_data(SParameter parameter, std::uint32_t expected_points)
{
    const std::string xax = query("CALC:DATA:XAX?");
    const auto axis = parse_ascii_doubles(xax);
    if (axis.size() < 2 || (expected_points > 0 && axis.size() != expected_points)) {
        throw std::runtime_error("C2220Vna: XAX length mismatch: got "
                                 + std::to_string(axis.size()) + ", expected "
                                 + std::to_string(expected_points));
    }

    const std::string sdat = query("CALC:DATA:SDAT?");
    const auto values = parse_ascii_doubles(sdat);
    if (values.size() != axis.size() * 2u) {
        throw std::runtime_error("C2220Vna: SDAT length mismatch: got "
                                 + std::to_string(values.size()) + " scalars, expected "
                                 + std::to_string(axis.size() * 2u));
    }

    ComplexSweep sweep;
    sweep.frequency_hz.resize(axis.size());
    sweep.overload = false;

    std::vector<std::complex<double>>* trace = nullptr;
    switch (parameter) {
    case SParameter::S11:
        trace = &sweep.s11;
        break;
    case SParameter::S21:
        trace = &sweep.s21;
        break;
    case SParameter::S12:
        trace = &sweep.s12;
        break;
    case SParameter::S22:
        trace = &sweep.s22;
        break;
    }
    trace->resize(axis.size());

    for (std::size_t i = 0; i < axis.size(); ++i) {
        if (!std::isfinite(axis[i]) || axis[i] < 0.0) {
            throw std::runtime_error("C2220Vna: invalid frequency axis value");
        }
        sweep.frequency_hz[i] = static_cast<std::uint64_t>(std::llround(axis[i]));
        (*trace)[i] = {values[i * 2u], values[i * 2u + 1u]};
    }
    return sweep;
}

ComplexSweep C2220Vna::measure_trace()
{
    require_connected("measure_trace");
    if (!configured_) {
        throw std::runtime_error("C2220Vna: measure_trace without configure");
    }

    transport_.set_io_timeout_ms(profile_.sweep_timeout_ms);

    std::runtime_error last{"C2220Vna: measure_trace failed"};
    const int attempts = profile_.measure_retries + 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        try {
            return measure_once();
        } catch (const std::runtime_error& ex) {
            last = ex;
        }
    }
    throw last;
}

ComplexSweep C2220Vna::measure_s21()
{
    require_connected("measure_s21");
    if (!configured_) {
        throw std::runtime_error("C2220Vna: measure_s21 without configure");
    }
    if (config_.s_parameter != SParameter::S21) {
        throw std::runtime_error("C2220Vna: measure_s21 requires s_parameter == S21");
    }
    return measure_trace();
}

ComplexSweep C2220Vna::read_current_trace(SweepConfig* instrument_config)
{
    require_connected("read_current_trace");
    transport_.set_io_timeout_ms(profile_.sweep_timeout_ms);

    const SweepConfig observed = read_config();
    ComplexSweep sweep = read_trace_data(observed.s_parameter, observed.points);
    if (instrument_config != nullptr) {
        *instrument_config = observed;
    }
    return sweep;
}

SweepConfig C2220Vna::read_config()
{
    require_connected("read_config");
    transport_.set_io_timeout_ms(profile_.sweep_timeout_ms);
    SweepConfig observed{};
    observed.f_start_hz = static_cast<std::uint64_t>(
        std::llround(parse_single_double(query("SENS:FREQ:STAR?"), "SENS:FREQ:STAR?")));
    observed.f_stop_hz = static_cast<std::uint64_t>(
        std::llround(parse_single_double(query("SENS:FREQ:STOP?"), "SENS:FREQ:STOP?")));
    observed.points = static_cast<std::uint32_t>(
        std::llround(parse_single_double(query("SENS:SWE:POIN?"), "SENS:SWE:POIN?")));
    observed.ifbw_hz = static_cast<std::uint32_t>(
        std::llround(parse_single_double(query("SENS:BAND?"), "SENS:BAND?")));
    observed.power_dbm = parse_single_double(query("SOUR:POW?"), "SOUR:POW?");
    observed.averages = scpi_state_is_on(query("SENS:AVER?"))
        ? static_cast<std::uint16_t>(std::llround(
              parse_single_double(query("SENS:AVER:COUN?"), "SENS:AVER:COUN?")))
        : 1;
    observed.s_parameter = parse_s_parameter(query("CALC:PAR:DEF?"));
    if (observed.points < 2 || observed.f_stop_hz < observed.f_start_hz) {
        throw std::runtime_error("C2220Vna: invalid current sweep settings");
    }

    return observed;
}

std::string C2220Vna::current_calibration_kit_id()
{
    require_connected("current_calibration_kit_id");
    const std::string index = trim_scpi_text(query("SENS:CORR:COLL:CKIT?"));
    const std::string label = trim_scpi_text(query("SENS:CORR:COLL:CKIT:LAB?"));
    if (index.empty()) {
        throw std::runtime_error("C2220Vna: empty calibration kit index");
    }
    return label.empty() ? "CKIT " + index : "CKIT " + index + " (" + label + ")";
}

std::string C2220Vna::select_calibration_kit(int index)
{
    require_connected("select_calibration_kit");
    if (index < 1 || index > 64) {
        throw std::runtime_error("C2220Vna: calibration kit index must be in range 1..64");
    }
    write_cmd("SENS:CORR:COLL:CKIT " + std::to_string(index));
    return current_calibration_kit_id();
}

void C2220Vna::select_calibration_subclass(int subclass)
{
    require_connected("select_calibration_subclass");
    if (subclass < 1 || subclass > 8) {
        throw std::runtime_error("C2220Vna: calibration subclass must be in range 1..8");
    }
    write_cmd("SENS:CORR:COLL:SUBC " + std::to_string(subclass));
    const std::string observed = trim_scpi_text(query("SENS:CORR:COLL:SUBC?"));
    int reported = 0;
    try {
        reported = std::stoi(observed);
    } catch (const std::exception&) {
        throw std::runtime_error("C2220Vna: invalid calibration subclass readback: " + observed);
    }
    if (reported != subclass) {
        throw std::runtime_error("C2220Vna: calibration subclass readback mismatch: " + observed);
    }
}

bool C2220Vna::correction_enabled()
{
    require_connected("correction_enabled");
    return scpi_state_is_on(query("SENS:CORR:STAT?"));
}

void C2220Vna::calibrate_one_port(OnePortCalibrationStep step, int port)
{
    require_connected("calibrate_one_port");
    if (port != 1 && port != 2) {
        throw std::runtime_error("C2220Vna: calibrate_one_port port must be 1 or 2");
    }
    std::string command;
    switch (step) {
    case OnePortCalibrationStep::Begin:
        command = "SENS:CORR:COLL:METH:SOLT1 " + std::to_string(port);
        break;
    case OnePortCalibrationStep::Open:
        command = "SENS:CORR:COLL:OPEN " + std::to_string(port);
        break;
    case OnePortCalibrationStep::Short:
        command = "SENS:CORR:COLL:SHOR " + std::to_string(port);
        break;
    case OnePortCalibrationStep::Load:
        command = "SENS:CORR:COLL:LOAD " + std::to_string(port);
        break;
    case OnePortCalibrationStep::Apply:
        command = "SENS:CORR:COLL:SAVE";
        break;
    }
    write_cmd(command);
    const std::string opc = query("*OPC?");
    if (opc.find('1') == std::string::npos) {
        throw std::runtime_error("C2220Vna: one-port calibration did not complete: " + opc);
    }
}

void C2220Vna::calibrate_two_port(TwoPortCalibrationStep step)
{
    require_connected("calibrate_two_port");
    const char* command = nullptr;
    switch (step) {
    case TwoPortCalibrationStep::Begin:
        command = "SENS:CORR:COLL:METH:SOLT2 1,2";
        break;
    case TwoPortCalibrationStep::OpenPort1:
        command = "SENS:CORR:COLL:OPEN 1";
        break;
    case TwoPortCalibrationStep::ShortPort1:
        command = "SENS:CORR:COLL:SHOR 1";
        break;
    case TwoPortCalibrationStep::LoadPort1:
        command = "SENS:CORR:COLL:LOAD 1";
        break;
    case TwoPortCalibrationStep::OpenPort2:
        command = "SENS:CORR:COLL:OPEN 2";
        break;
    case TwoPortCalibrationStep::ShortPort2:
        command = "SENS:CORR:COLL:SHOR 2";
        break;
    case TwoPortCalibrationStep::LoadPort2:
        command = "SENS:CORR:COLL:LOAD 2";
        break;
    case TwoPortCalibrationStep::Thru12:
        command = "SENS:CORR:COLL:THRU 2,1";
        break;
    case TwoPortCalibrationStep::Apply:
        command = "SENS:CORR:COLL:SAVE";
        break;
    }
    write_cmd(command);
    const std::string opc = query("*OPC?");
    if (opc.find('1') == std::string::npos) {
        throw std::runtime_error("C2220Vna: calibration did not complete: " + opc);
    }
}

std::vector<std::string> C2220Vna::drain_errors()
{
    require_connected("drain_errors");
    std::vector<std::string> errors;
    // Защита от бесконечного цикла при битом приборе.
    constexpr int kMax = 64;
    for (int i = 0; i < kMax; ++i) {
        const std::string line = query("SYST:ERR?");
        if (is_no_error_response(line)) {
            break;
        }
        errors.push_back(line);
    }
    return errors;
}

void C2220Vna::abort() noexcept
{
    connected_ = false;
    configured_ = false;
    try {
        transport_.abort();
    } catch (...) {
        // noexcept: глотаем всё.
    }
}
