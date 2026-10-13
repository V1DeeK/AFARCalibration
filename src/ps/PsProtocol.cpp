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

// ------------------------------------------------- внутренние модули (§4.5)

namespace {

bool isHalfStep(double v) noexcept
{
    const double twice = v * 2.0;
    return std::fabs(twice - std::round(twice)) < 1e-9;
}

const ModuleField* findField(const std::vector<ModuleField>& fields, std::string_view key)
{
    const auto it = std::find_if(fields.begin(), fields.end(),
                                 [key](const ModuleField& f) { return f.key == key; });
    return it == fields.end() ? nullptr : &*it;
}

/// Поля из values в p с проверкой по таблице полей модуля.
bool putValues(const std::vector<ModuleField>& fields, const ModuleValues& values, json& p,
               const std::string& where, std::string* error)
{
    if (values.empty()) {
        setError(error, where + ": не задано ни одного поля");
        return false;
    }
    for (const auto& [key, value] : values) {
        const ModuleField* f = findField(fields, key);
        if (!f) {
            setError(error, where + ": нет поля " + key);
            return false;
        }
        if (!moduleValueValid(f->kind, value)) {
            setError(error, where + ": " + key + " — допустимо " + moduleRangeText(f->kind));
            return false;
        }
        p[key] = moduleFieldValue(f->kind, value);
    }
    return true;
}

/// Известные поля из ответа; значение вне допустимых — ошибка разбора.
bool readValues(const json& o, const std::vector<ModuleField>& fields, ModuleValues& out,
                std::string* error)
{
    for (const auto& f : fields) {
        const auto it = o.find(f.key);
        if (it == o.end()) {
            continue;
        }
        double v = 0.0;
        if (!readNumber(o, f.key, v) || !moduleValueValid(f.kind, v)) {
            setError(error, "недопустимое значение " + std::string(f.key));
            return false;
        }
        out[std::string(f.key)] = v;
    }
    return true;
}

}  // namespace

const std::vector<ModuleField>& loFields()
{
    using K = ModuleFieldKind;
    static const std::vector<ModuleField> lo = {
        {"pwr_b", K::PowerDbm, "Мощность RF_OUT_B, дБм"},
        {"pwr_a", K::PowerDbm, "Мощность RF_OUT_A, дБм"},
        {"en_b", K::Flag, "Выход RF_OUT_B"},
        {"en_a", K::Flag, "Выход RF_OUT_A"},
        {"en_rf", K::Flag, "Все РЧ-выходы"},
        {"en_syn", K::Flag, "Синтезатор ADF4355"},
        {"ref", K::Flag, "Внешняя опорная частота"},
        {"freq", K::FrequencyHz, "Частота синтезатора, Гц"},
        {"att1", K::Att31, "Аттенюация канала 1, дБ"},
        {"att2", K::Att31, "Аттенюация канала 2, дБ"},
    };
    return lo;
}

const std::vector<ModuleField>& videoFields()
{
    static const std::vector<ModuleField> video = [] {
        auto v = loFields();
        v.push_back({"att_mode", ModuleFieldKind::Flag, "Ручной режим аттенюации"});
        v.push_back({"en_vid", ModuleFieldKind::Flag, "Видеоприёмник"});
        return v;
    }();
    return video;
}

const std::vector<ModuleField>& mixerChannelFields()
{
    using K = ModuleFieldKind;
    static const std::vector<ModuleField> mixer = {
        {"en", K::Flag, "Канал включён"},
        {"rp", K::Flag, "Пониженная мощность"},
        {"if_att", K::Att15, "Аттенюатор ПЧ, дБ"},
        {"att", K::Att31, "Аттенюация входа, дБ"},
    };
    return mixer;
}

bool moduleValueValid(ModuleFieldKind kind, double v) noexcept
{
    if (!std::isfinite(v)) {
        return false;
    }
    switch (kind) {
    case ModuleFieldKind::Flag: return v == 0.0 || v == 1.0;
    case ModuleFieldKind::PowerDbm: return v == -4.0 || v == -1.0 || v == 2.0 || v == 5.0;
    case ModuleFieldKind::FrequencyHz: return v >= 0.0 && v <= 7.0e9 && std::trunc(v) == v;
    case ModuleFieldKind::Att31: return v >= 0.0 && v <= 31.5 && isHalfStep(v);
    case ModuleFieldKind::Att15: return v >= 0.0 && v <= 15.5 && isHalfStep(v);
    }
    return false;
}

std::string moduleRangeText(ModuleFieldKind kind)
{
    switch (kind) {
    case ModuleFieldKind::Flag: return "0 или 1";
    case ModuleFieldKind::PowerDbm: return "−4, −1, 2, 5";
    case ModuleFieldKind::FrequencyHz: return "0…7000000000, целое";
    case ModuleFieldKind::Att31: return "0…31,5 шаг 0,5";
    case ModuleFieldKind::Att15: return "0…15,5 шаг 0,5";
    }
    return {};
}

nlohmann::json moduleFieldValue(ModuleFieldKind kind, double v)
{
    switch (kind) {
    case ModuleFieldKind::Flag:
    case ModuleFieldKind::PowerDbm:
    case ModuleFieldKind::FrequencyHz: return static_cast<std::int64_t>(std::llround(v));
    case ModuleFieldKind::Att31:
    case ModuleFieldKind::Att15: return v;
    }
    return v;
}

std::string loNameRu(int addr)
{
    switch (addr) {
    case kLoMixer: return "Гетеродин смесителя";
    case kLoCalibration: return "Гетеродин калибровки";
    default: return "Гетеродин " + std::to_string(addr);
    }
}

bool loSetPayload(int addr, const ModuleValues& values, nlohmann::json& payload, std::string* error)
{
    if (addr != kLoMixer && addr != kLoCalibration) {
        setError(error, "адрес гетеродина — 1 или 2");
        return false;
    }
    json p{{"addr", addr}};
    if (!putValues(loFields(), values, p, loNameRu(addr), error)) {
        return false;
    }
    payload = std::move(p);
    return true;
}

nlohmann::json loGetPayload(int addr)
{
    return json{{"addr", addr}};
}

bool videoSetPayload(const ModuleValues& values, nlohmann::json& payload, std::string* error)
{
    json p = json::object();
    if (!putValues(videoFields(), values, p, "Видеоприёмник", error)) {
        return false;
    }
    payload = std::move(p);
    return true;
}

bool mixerSetPayload(const std::vector<MixerChannel>& channels, nlohmann::json& payload,
                     std::string* error)
{
    if (channels.empty() || channels.size() > static_cast<std::size_t>(kMixerChannels)) {
        setError(error, "смеситель: от 1 до 4 каналов");
        return false;
    }
    std::vector<MixerChannel> sorted = channels;
    std::sort(sorted.begin(), sorted.end(),
              [](const MixerChannel& a, const MixerChannel& b) { return a.n < b.n; });
    json ch = json::array();
    int prev = 0;
    for (const auto& c : sorted) {
        if (c.n < 1 || c.n > kMixerChannels || c.n == prev) {
            setError(error, "смеситель: номер канала 1…4 без повторов");
            return false;
        }
        prev = c.n;
        json e{{"n", c.n}};
        if (!putValues(mixerChannelFields(), c.values, e, "смеситель, канал " + std::to_string(c.n),
                       error)) {
            return false;
        }
        ch.push_back(std::move(e));
    }
    payload = json{{"ch", std::move(ch)}};
    return true;
}

bool ModuleState::fromJson(const nlohmann::json& o, const std::vector<ModuleField>& fields,
                           ModuleState& out, std::string* error)
{
    if (!o.is_object()) {
        setError(error, "payload не объект");
        return false;
    }
    ModuleState s;
    std::int64_t addr = 0;
    if (readInt(o, "addr", addr)) {
        s.addr = static_cast<int>(std::clamp<std::int64_t>(addr, 0, 255));
    }
    if (!readValues(o, fields, s.values, error)) {
        return false;
    }
    double temp = 0.0;
    if (readNumber(o, "temp", temp)) {
        s.temp = temp;
    }
    std::int64_t lock = 0;
    if (readInt(o, "lock", lock)) {
        s.lock = static_cast<int>(lock != 0);
    }
    out = std::move(s);
    return true;
}

bool MixerState::fromJson(const nlohmann::json& o, MixerState& out, std::string* error)
{
    const auto it = o.find("ch");
    if (it == o.end() || !it->is_array() || it->empty() ||
        it->size() > static_cast<std::size_t>(kMixerChannels)) {
        setError(error, "ch — массив из 1…4 каналов смесителя");
        return false;
    }
    MixerState s;
    std::set<int> seen;
    for (const auto& e : *it) {
        std::int64_t n = 0;
        if (!e.is_object() || !readInt(e, "n", n) || n < 1 || n > kMixerChannels ||
            !seen.insert(static_cast<int>(n)).second) {
            setError(error, "канал смесителя: n 1…4 без повторов");
            return false;
        }
        MixerChannel c;
        c.n = static_cast<int>(n);
        if (!readValues(e, mixerChannelFields(), c.values, error)) {
            return false;
        }
        s.ch.push_back(std::move(c));
    }
    std::sort(s.ch.begin(), s.ch.end(), [](const MixerChannel& a, const MixerChannel& b) { return a.n < b.n; });
    double temp = 0.0;
    if (readNumber(o, "temp", temp)) {
        s.temp = temp;
    }
    out = std::move(s);
    return true;
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

// ---------------------------------------------------- полное сканирование

bool SffParams::validate(std::string* error) const
{
    if (freq_min <= 0 || freq_max > kFrequencyLimitHz || freq_min >= freq_max) {
        setError(error, "частоты обзора: нужно 0 < от < до ≤ 100 ГГц");
        return false;
    }
    if (az_min < -kAzimuthLimitDeg || az_max > kAzimuthLimitDeg || az_min >= az_max) {
        setError(error, "сектор обзора: нужно −30° ≤ от < до ≤ +30°");
        return false;
    }
    if (period < 0 || period > kSffPeriodMaxSec) {
        setError(error, "период обзора: 0…3600 с");
        return false;
    }
    return true;
}

nlohmann::json SffParams::toJson() const
{
    return json{{"freq_min", freq_min}, {"freq_max", freq_max}, {"az_min", az_min},
                {"az_max", az_max},     {"period", period}};
}

bool SffParams::fromJson(const nlohmann::json& o, SffParams& out, std::string* error)
{
    SffParams p;
    std::int64_t period = 0;
    if (!readInt(o, "freq_min", p.freq_min) || !readInt(o, "freq_max", p.freq_max) ||
        !readNumber(o, "az_min", p.az_min) || !readNumber(o, "az_max", p.az_max) ||
        !readInt(o, "period", period)) {
        setError(error, "нет параметров обзора freq_min, freq_max, az_min, az_max, period");
        return false;
    }
    p.period = static_cast<int>(std::clamp<std::int64_t>(period, -1, kSffPeriodMaxSec + 1));
    if (!p.validate(error)) {
        return false;
    }
    out = p;
    return true;
}

bool SffResult::peak(double& power, double& azimuth) const
{
    bool any = false;
    for (const auto& point : fft) {
        for (double v : point.pwr) {
            if (!any || v > power) {
                power = v;
                azimuth = point.az;
                any = true;
            }
        }
    }
    return any;
}

bool SffResult::fromJson(const nlohmann::json& o, SffResult& out, std::string* error)
{
    SffResult r;
    std::string cal;
    if (!readInt(o, "freq", r.freq) || r.freq <= 0 || !readString(o, "cal", cal) ||
        (cal != "OK" && cal != "NO")) {
        setError(error, "send_sff_result: нужны freq > 0 и cal \"OK\"/\"NO\"");
        return false;
    }
    r.cal = cal == "OK";
    const auto it = o.find("fft");
    if (it == o.end() || !it->is_array()) {
        setError(error, "send_sff_result: нет массива fft");
        return false;
    }
    for (const auto& item : *it) {
        SffPoint point;
        if (!item.is_object() || !readNumber(item, "az", point.az)) {
            setError(error, "send_sff_result: элемент fft без az");
            return false;
        }
        if (const auto pw = item.find("pwr"); pw != item.end() && pw->is_array()) {
            for (const auto& v : *pw) {
                if (v.is_number()) {
                    point.pwr.push_back(v.get<double>());
                }
            }
        }
        r.fft.push_back(std::move(point));
    }
    out = std::move(r);
    return true;
}

}  // namespace afar::ps
