#include "C2220Vna.h"

#include <charconv>
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

bool direct_access_is_on(const std::string& reply)
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
        const char* first = token.data();
        const char* last = token.data() + token.size();
        const auto [ptr, ec] = std::from_chars(first, last, value);
        if (ec != std::errc{} || ptr != last) {
            // from_chars может не принять экспоненту на libstdc++ — fallback.
            try {
                std::size_t consumed = 0;
                value = std::stod(token, &consumed);
                if (consumed != token.size()) {
                    throw std::runtime_error("bad token");
                }
            } catch (...) {
                throw std::runtime_error("C2220Vna: cannot parse number '" + token + "'");
            }
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
    if (direct_access_is_on(reply) && !profile_.allow_direct_access) {
        throw std::runtime_error(
            "C2220Vna: direct receiver access is ON but profile.allow_direct_access is false");
    }
}

void C2220Vna::connect()
{
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
    return idn;
}

void C2220Vna::configure(const SweepConfig& config)
{
    require_connected("configure");
    if (config.points < 2) {
        throw std::invalid_argument("C2220Vna: points must be >= 2");
    }
    if (config.f_stop_hz < config.f_start_hz) {
        throw std::invalid_argument("C2220Vna: f_stop_hz < f_start_hz");
    }

    write_cmd("SENS:FREQ:STAR " + std::to_string(config.f_start_hz));
    write_cmd("SENS:FREQ:STOP " + std::to_string(config.f_stop_hz));
    write_cmd("SENS:SWE:POIN " + std::to_string(config.points));
    write_cmd("SENS:BAND " + std::to_string(config.ifbw_hz));
    write_cmd("SOUR:POW " + format_double(config.power_dbm));
    write_cmd(std::string("CALC:PAR:DEF ") + s_parameter_scpi(config.s_parameter));

    config_ = config;
    configured_ = true;
}

ComplexSweep C2220Vna::measure_once()
{
    write_cmd("TRIG:SING");
    const std::string opc = query("*OPC?");
    if (opc.find('1') == std::string::npos) {
        throw std::runtime_error("C2220Vna: unexpected *OPC? reply: " + opc);
    }

    const std::string sdat = query("CALC:DATA:SDAT?");
    const auto values = parse_ascii_doubles(sdat);
    if (values.size() != static_cast<std::size_t>(config_.points) * 2u) {
        throw std::runtime_error("C2220Vna: SDAT length mismatch: got "
                                 + std::to_string(values.size()) + " scalars, expected "
                                 + std::to_string(static_cast<std::size_t>(config_.points) * 2u));
    }

    const std::string xax = query("CALC:DATA:XAX?");
    const auto axis = parse_ascii_doubles(xax);
    if (axis.size() != config_.points) {
        throw std::runtime_error("C2220Vna: XAX length mismatch: got "
                                 + std::to_string(axis.size()) + ", expected "
                                 + std::to_string(config_.points));
    }

    ComplexSweep sweep;
    sweep.frequency_hz.resize(config_.points);
    sweep.overload = false;

    std::vector<std::complex<double>>* trace = nullptr;
    switch (config_.s_parameter) {
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
    trace->resize(config_.points);

    for (std::uint32_t i = 0; i < config_.points; ++i) {
        sweep.frequency_hz[i] = static_cast<std::uint64_t>(axis[i] + 0.5);
        (*trace)[i] = {values[static_cast<std::size_t>(i) * 2u],
                       values[static_cast<std::size_t>(i) * 2u + 1u]};
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
