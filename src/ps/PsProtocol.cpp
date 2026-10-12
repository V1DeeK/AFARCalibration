#include "PsProtocol.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <set>

namespace afar::ps {

namespace {

using nlohmann::json;

void setError(std::string* error, std::string text)
{
    if (error) {
        *error = std::move(text);
    }
}

/// Целое из JSON-числа без дробной части.
bool readInt(const json& o, std::string_view key, std::int64_t& out)
{
    const auto it = o.find(key);
    if (it == o.end()) {
        return false;
    }
    if (it->is_number_integer()) {
        out = it->get<std::int64_t>();
        return true;
    }
    if (it->is_number_float()) {
        const double v = it->get<double>();
        if (!std::isfinite(v) || std::trunc(v) != v || std::fabs(v) > 9.0e18) {
            return false;
        }
        out = static_cast<std::int64_t>(v);
        return true;
    }
    return false;
}

bool readNumber(const json& o, std::string_view key, double& out)
{
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number()) {
        return false;
    }
    out = it->get<double>();
    return std::isfinite(out);
}

bool readString(const json& o, std::string_view key, std::string& out)
{
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

}  // namespace

std::string_view toString(MsgClass v) noexcept
{
    switch (v) {
    case MsgClass::Command: return "command";
    case MsgClass::Response: return "response";
    case MsgClass::Event: return "event";
    }
    return "command";
}

std::string_view toString(Status v) noexcept
{
    switch (v) {
    case Status::Ok: return "OK";
    case Status::Busy: return "BUSY";
    case Status::NotSupported: return "NOT_SUPPORTED";
    case Status::InvalidArgument: return "INVALID_ARGUMENT";
    case Status::HwNotReady: return "HW_NOT_READY";
    }
    return "OK";
}

std::string_view toString(Mode v) noexcept
{
    switch (v) {
    case Mode::Idle: return "IDLE";
    case Mode::FullScanFreq: return "FULL_SCAN_FREQ";
    case Mode::Operational: return "OPERATIONAL";
    case Mode::Service: return "SERVICE";
    }
    return "IDLE";
}

std::string_view toString(DeviceHealth v) noexcept
{
    switch (v) {
    case DeviceHealth::Ok: return "OK";
    case DeviceHealth::Fail: return "FAIL";
    case DeviceHealth::NotChecked: return "NOT_CHECKED";
    }
    return "NOT_CHECKED";
}

bool parseMsgClass(std::string_view s, MsgClass& out) noexcept
{
    for (const auto v : {MsgClass::Command, MsgClass::Response, MsgClass::Event}) {
        if (toString(v) == s) {
            out = v;
            return true;
        }
    }
    return false;
}

bool parseStatus(std::string_view s, Status& out) noexcept
{
    for (const auto v : {Status::Ok, Status::Busy, Status::NotSupported, Status::InvalidArgument,
                         Status::HwNotReady}) {
        if (toString(v) == s) {
            out = v;
            return true;
        }
    }
    return false;
}

bool parseMode(std::string_view s, Mode& out) noexcept
{
    for (const auto v : {Mode::Idle, Mode::FullScanFreq, Mode::Operational, Mode::Service}) {
        if (toString(v) == s) {
            out = v;
            return true;
        }
    }
    return false;
}

std::string statusRu(Status v)
{
    switch (v) {
    case Status::Ok: return "выполнено";
    case Status::Busy: return "занято";
    case Status::NotSupported: return "не поддерживается";
    case Status::InvalidArgument: return "недопустимые параметры";
    case Status::HwNotReady: return "аппаратура не готова";
    }
    return {};
}

std::string modeRu(Mode v)
{
    switch (v) {
    case Mode::Idle: return "ожидание";
    case Mode::FullScanFreq: return "полное сканирование";
    case Mode::Operational: return "оперативный";
    case Mode::Service: return "сервисный";
    }
    return {};
}

std::string stateRu(std::string_view state)
{
    static const std::map<std::string_view, std::string_view> names = {
        {"INIT", "инициализация"},
        {"SELF_TEST", "самопроверка"},
        {"READY", "готов"},
        {"FAULT", "неисправность"},
        {"SFF_IDLE", "ожидание команды"},
        {"SFF_OBSERVATION", "обзор"},
    };
    const auto it = names.find(state);
    return it == names.end() ? std::string(state) : std::string(it->second);
}

bool versionsCompatible(std::string_view a, std::string_view b)
{
    const auto major = [](std::string_view v) { return v.substr(0, v.find('.')); };
    return !a.empty() && !b.empty() && major(a) == major(b);
}

std::string encodeDatagram(const Envelope& env)
{
    json o;
    o["msg_type"] = env.msg_type;
    o["msg_class"] = std::string(toString(env.msg_class));
    o["msg_count"] = env.msg_count;
    o["payload"] = env.payload.is_object() ? env.payload : json::object();
    // dump() не вставляет переводы строк; управляющие символы экранирует.
    return o.dump(-1, ' ', false, json::error_handler_t::replace);
}

std::string encodeLine(const Envelope& env)
{
    return encodeDatagram(env) + '\n';
}

bool decodeLine(std::string_view line, Envelope& out, std::string* error)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    const json o = json::parse(line, nullptr, false);
    if (o.is_discarded() || !o.is_object()) {
        setError(error, "строка не является объектом JSON");
        return false;
    }
    Envelope env;
    if (!readString(o, "msg_type", env.msg_type) || env.msg_type.empty()) {
        setError(error, "нет поля msg_type");
        return false;
    }
    std::string cls;
    if (!readString(o, "msg_class", cls) || !parseMsgClass(cls, env.msg_class)) {
        setError(error, "неверное поле msg_class");
        return false;
    }
    std::int64_t count = 0;
    if (!readInt(o, "msg_count", count) || count < 0 || count > 255) {
        setError(error, "msg_count вне 0…255");
        return false;
    }
    env.msg_count = static_cast<std::uint8_t>(count);
    const auto payload = o.find("payload");
    if (payload == o.end() || !payload->is_object()) {
        setError(error, "payload не объект");
        return false;
    }
    env.payload = *payload;
    out = std::move(env);
    return true;
}

Envelope makeCommand(std::string_view msgType, std::uint8_t count, nlohmann::json payload)
{
    Envelope env;
    env.msg_type = std::string(msgType);
    env.msg_class = MsgClass::Command;
    env.msg_count = count;
    env.payload = payload.is_object() ? std::move(payload) : json::object();
    return env;
}

bool responseStatus(const Envelope& response, Status& out) noexcept
{
    const auto it = response.payload.find("status");
    if (it == response.payload.end() || !it->is_string()) {
        return false;
    }
    return parseStatus(it->get_ref<const std::string&>(), out);
}

bool LineSplitter::append(std::string_view bytes)
{
    buffer_.append(bytes);
    // Каждая строка в буфере, включая незавершённый хвост, не длиннее предела.
    std::size_t start = 0;
    while (start <= buffer_.size()) {
        const auto nl = buffer_.find('\n', start);
        const std::size_t end = nl == std::string::npos ? buffer_.size() : nl;
        if (end - start > kMaxLineBytes) {
            buffer_.clear();
            return false;
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return true;
}

bool LineSplitter::takeLine(std::string& line)
{
    const auto nl = buffer_.find('\n');
    if (nl == std::string::npos) {
        return false;
    }
    line.assign(buffer_, 0, nl);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    buffer_.erase(0, nl + 1);
    return true;
}

// ------------------------------------------------------------------ payload

nlohmann::json Hello::toJson() const
{
    return json{{"ver", ver}, {"tlm_ip", tlm_ip}, {"tlm_port", tlm_port}};
}

bool StateInfo::fromJson(const nlohmann::json& o, StateInfo& out, std::string* error)
{
    std::string mode;
    if (!readString(o, "mode", mode) || !parseMode(mode, out.mode)) {
        setError(error, "неизвестный режим mode");
        return false;
    }
    if (!readString(o, "state", out.state)) {
        setError(error, "нет поля state");
        return false;
    }
    return true;
}

namespace {

bool checkChannels(std::vector<ChannelCodes>& channels, std::string* error)
{
    if (channels.size() != static_cast<std::size_t>(kChannels)) {
        setError(error, "в ch должно быть ровно 16 каналов, получено " +
                            std::to_string(channels.size()));
        return false;
    }
    std::set<int> seen;
    for (const auto& c : channels) {
        if (c.n < 1 || c.n > kChannels) {
            setError(error, "номер канала " + std::to_string(c.n) + " вне 1…16");
            return false;
        }
        if (!seen.insert(c.n).second) {
            setError(error, "канал " + std::to_string(c.n) + " повторяется");
            return false;
        }
        if (c.ph < 0 || c.ph > kCodeMax || c.att < 0 || c.att > kCodeMax) {
            setError(error, "код канала " + std::to_string(c.n) + " вне 0…63");
            return false;
        }
    }
    std::sort(channels.begin(), channels.end(),
              [](const ChannelCodes& a, const ChannelCodes& b) { return a.n < b.n; });
    return true;
}

}  // namespace

bool channelsFromJson(const nlohmann::json& payload, std::vector<ChannelCodes>& out,
                      std::string* error)
{
    const auto it = payload.find("ch");
    if (it == payload.end() || !it->is_array()) {
        setError(error, "нет массива ch");
        return false;
    }
    std::vector<ChannelCodes> channels;
    channels.reserve(it->size());
    for (const auto& e : *it) {
        std::int64_t n = 0;
        std::int64_t ph = 0;
        std::int64_t att = 0;
        if (!e.is_object() || !readInt(e, "n", n) || !readInt(e, "ph", ph) ||
            !readInt(e, "att", att) || n < -1 || n > 1000 || ph < -1 || ph > 1000 || att < -1 ||
            att > 1000) {
            setError(error, "элемент ch без целых n, ph, att");
            return false;
        }
        channels.push_back({static_cast<int>(n), static_cast<int>(ph), static_cast<int>(att)});
    }
    if (!checkChannels(channels, error)) {
        return false;
    }
    out = std::move(channels);
    return true;
}

bool channelsToJson(const std::vector<ChannelCodes>& channels, nlohmann::json& payload,
                    std::string* error)
{
    std::vector<ChannelCodes> sorted = channels;
    if (!checkChannels(sorted, error)) {
        return false;
    }
    json arr = json::array();
    for (const auto& c : sorted) {
        arr.push_back(json{{"n", c.n}, {"ph", c.ph}, {"att", c.att}});
    }
    payload = json{{"ch", std::move(arr)}};
    return true;
}

bool AdcParams::validate(std::string* error) const
{
    if (lo <= 0 || lo > kFrequencyLimitHz) {
        setError(error, "частота гетеродина lo вне (0; 100 ГГц]");
        return false;
    }
    if (bw <= 0 || bw > kFrequencyLimitHz) {
        setError(error, "полоса bw должна быть больше 0");
        return false;
    }
    if (fs <= 0 || fs > kFrequencyLimitHz) {
        setError(error, "частота дискретизации fs должна быть больше 0");
        return false;
    }
    if (!std::isfinite(gain)) {
        setError(error, "усиление gain не число");
        return false;
    }
    return true;
}

nlohmann::json AdcParams::toJson() const
{
    return json{{"lo", lo}, {"bw", bw}, {"fs", fs}, {"gain", gain}};
}

bool AdcParams::fromJson(const nlohmann::json& o, AdcParams& out, std::string* error)
{
    AdcParams p;
    if (!readInt(o, "lo", p.lo) || !readInt(o, "bw", p.bw) || !readInt(o, "fs", p.fs) ||
        !readNumber(o, "gain", p.gain)) {
        setError(error, "нет целых lo, bw, fs или числа gain");
        return false;
    }
    if (!p.validate(error)) {
        return false;
    }
    out = p;
    return true;
}

bool BeamState::fromJson(const nlohmann::json& o, BeamState& out, std::string* error)
{
    std::string src;
    if (!readString(o, "src", src)) {
        setError(error, "нет поля src");
        return false;
    }
    BeamState b;
    if (src == "azimuth") {
        b.src = BeamSource::Azimuth;
        if (!readNumber(o, "az", b.az)) {
            setError(error, "нет поля az");
            return false;
        }
    } else if (src == "table") {
        b.src = BeamSource::Table;
        std::int64_t beam = 0;
        if (!readInt(o, "beam", beam) || beam < 0 || beam > 1'000'000) {
            setError(error, "нет поля beam");
            return false;
        }
        b.beam = static_cast<int>(beam);
    } else if (src == "codes") {
        b.src = BeamSource::Codes;
    } else if (src == "none") {
        b.src = BeamSource::None;
    } else {
        setError(error, "неизвестный src: " + src);
        return false;
    }
    out = b;
    return true;
}

bool hexBytesValid(std::string_view hex) noexcept
{
    if (hex.empty() || hex.size() % 2 != 0) {
        return false;
    }
    return std::all_of(hex.begin(), hex.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// ------------------------------------------------------------- телеметрия

bool Telemetry::fromJson(const nlohmann::json& o, Telemetry& out, std::string* error)
{
    Telemetry t;
    std::string mode;
    if (!readInt(o, "time", t.time) || !readInt(o, "uptime", t.uptime) ||
        !readString(o, "mode", mode) || !parseMode(mode, t.mode) ||
        !readString(o, "state", t.state)) {
        setError(error, "нет обязательных полей телеметрии");
        return false;
    }
    if (const auto it = o.find("devices"); it != o.end() && it->is_object()) {
        for (const auto& [id, value] : it->items()) {
            DeviceHealth h = DeviceHealth::NotChecked;
            if (value == "OK") {
                h = DeviceHealth::Ok;
            } else if (value == "FAIL") {
                h = DeviceHealth::Fail;
            }
            t.devices.emplace_back(id, h);
        }
    }
    const auto numbers = [&o](std::string_view key, std::vector<std::pair<std::string, double>>& to) {
        if (const auto it = o.find(key); it != o.end() && it->is_object()) {
            for (const auto& [id, value] : it->items()) {
                if (value.is_number()) {
                    to.emplace_back(id, value.get<double>());
                }
            }
        }
    };
    numbers("temperature", t.temperature);
    numbers("power", t.power);
    out = std::move(t);
    return true;
}

std::string selfTestRu(std::string_view id)
{
    static const std::map<std::string_view, std::string_view> names = {
        {"power", "питание"},
        {"temperature", "температура"},
        {"pl", "ПЛИС"},
        {"synth", "синтезатор"},
        {"adc", "АЦП"},
        {"beamformer", "ФВ и аттенюаторы"},
        {"calibration", "калибровочные таблицы"},
        {"rs485", "RS-485"},
    };
    const auto it = names.find(id);
    return it == names.end() ? std::string(id) : std::string(it->second);
}

}  // namespace afar::ps
