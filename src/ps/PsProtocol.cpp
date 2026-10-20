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
        // v8 таблица 11.
        {"SFF_IDLE", "ожидание команды"},
        {"SFF_FREQ", "установка частоты"},
        {"SFF_CALIBR", "применение калибровки"},
        {"SFF_AZIMUTH", "установка азимута"},
        {"SFF_FFT", "вычисление мощности в подполосах"},
        {"OP_INITIALIZATION", "установка частоты"},
        {"OP_CALIBRATION", "калибровка каналов"},
        {"OP_IDLE", "ожидание команды"},
        {"OP_SURVEY", "поиск"},
        {"OP_ACQUIRE", "захват"},
        {"OP_TRACK", "сопровождение"},
        {"OP_LOSS", "потеря цели"},
        {"OP_LIMITED_SURVEY", "ограниченный поиск"},
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
        if (c.ph < 0 || c.ph > kCodeMax || c.att < 0 || c.att > kCodeMax ||
            (c.en != 0 && c.en != 1)) {
            setError(error, "коды канала " + std::to_string(c.n) + " вне допустимого диапазона");
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
        std::int64_t en = 0;
        if (!e.is_object() || !readInt(e, "n", n) || !readInt(e, "ph", ph) ||
            !readInt(e, "att", att) || !readInt(e, "en", en) || n < -1 || n > 1000 ||
            ph < -1 || ph > 1000 || att < -1 || att > 1000 || en < 0 || en > 1) {
            setError(error, "элемент ch без допустимых n, ph, att, en");
            return false;
        }
        channels.push_back({static_cast<int>(n), static_cast<int>(ph), static_cast<int>(att),
                            static_cast<int>(en)});
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
        arr.push_back(json{{"n", c.n}, {"ph", c.ph}, {"att", c.att}, {"en", c.en}});
    }
    payload = json{{"ch", std::move(arr)}};
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
    if (src == "AZIMUTH") {
        b.src = BeamSource::Azimuth;
        if (!readNumber(o, "az", b.az)) {
            setError(error, "нет поля az");
            return false;
        }
    } else if (src == "TABLE") {
        b.src = BeamSource::Table;
        std::int64_t beam = 0;
        if (!readInt(o, "beam", beam) || beam < 0 || beam > 1'000'000) {
            setError(error, "нет поля beam");
            return false;
        }
        b.beam = static_cast<int>(beam);
    } else if (src == "CODES") {
        b.src = BeamSource::Codes;
    } else if (src == "NONE") {
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
    std::int64_t pres = 0;
    if (readInt(o, "pres", pres)) {
        s.pres = static_cast<int>(pres != 0);
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

bool Telemetry::fromStatusBytes(std::string_view bytes, Telemetry& out)
{
    if (bytes.size() != 10) {
        return false;
    }
    static constexpr std::array<std::string_view, 80> names = {
        "Цифровой модуль: VCCINT", "Цифровой модуль: VCCAUX",
        "Цифровой модуль: VBRAM", "Цифровой модуль: температура < 85 °C",
        "Цифровой модуль: температура < 125 °C", "", "", "",
        "Трансивер: связь", "Трансивер: тактирование", "Трансивер: гетеродин RX",
        "Трансивер: калибровка", "Трансивер: канал данных АЦП",
        "Трансивер: проверка данных", "", "",
        "Трансивер: тактовый сигнал", "Трансивер: LOCK",
        "Трансивер: температура < 85 °C", "Трансивер: температура < 125 °C",
        "", "", "", "",
        "Гетеродин смесителя: RS-485", "Гетеродин смесителя: LOCK",
        "Гетеродин смесителя: температура < 60 °C", "Гетеродин смесителя: температура < 80 °C",
        "", "", "", "",
        "Гетеродин калибровки: RS-485", "Гетеродин калибровки: LOCK",
        "Гетеродин калибровки: температура < 60 °C", "Гетеродин калибровки: температура < 80 °C",
        "", "", "", "",
        "Смеситель: RS-485", "Смеситель: температура < 60 °C",
        "Смеситель: температура < 80 °C", "", "", "", "", "",
        "РЧ-модуль 1: температура < 60 °C", "РЧ-модуль 1: температура < 80 °C",
        "РЧ-модуль 2: температура < 60 °C", "РЧ-модуль 2: температура < 80 °C",
        "РЧ-модуль 3: температура < 60 °C", "РЧ-модуль 3: температура < 80 °C",
        "РЧ-модуль 4: температура < 60 °C", "РЧ-модуль 4: температура < 80 °C",
        "Видеоприёмник: RS-485", "Видеоприёмник: LOCK",
        "Видеоприёмник: температура < 60 °C", "Видеоприёмник: температура < 80 °C",
        "Видеоприёмник: уровень сигнала", "", "", "",
        "Коммутационный модуль: температура < 60 °C",
        "Коммутационный модуль: температура < 80 °C",
        "Коммутационный модуль: влажность < 40 %",
        "Коммутационный модуль: влажность < 70 %", "", "", "", "",
        "Самопроверка: питание", "Самопроверка: ПЛИС",
        "Самопроверка: трансивер", "Самопроверка: RS-485",
        "Самопроверка: температура", "Самопроверка: синтезаторы",
        "Самопроверка: IQ", "Самопроверка: калибровочные таблицы",
    };
    Telemetry t;
    for (std::size_t i = 0; i < t.statusBytes.size(); ++i) {
        t.statusBytes[i] = static_cast<std::uint8_t>(bytes[i]);
    }
    for (std::size_t bit = 0; bit < names.size(); ++bit) {
        if (!names[bit].empty()) {
            // Все биты: 1 — исправно (таблица 5 протокола Пеленг-FPV-49-60, в. 1);
            // биты 14 и 15 — резерв.
            const bool ok = (t.statusBytes[bit / 8] & (1u << (bit % 8))) != 0;
            t.devices.emplace_back(std::string(names[bit]), ok ? DeviceHealth::Ok : DeviceHealth::Fail);
        }
    }
    out = std::move(t);
    return true;
}

bool Telemetry::fromJson(const nlohmann::json& o, Telemetry& out, std::string* error)
{
    if (!o.is_object()) {
        setError(error, "телеметрия: payload не объект");
        return false;
    }
    Telemetry t;
    std::string mode;
    readInt(o, "time", t.time);
    readInt(o, "uptime", t.uptime);
    readString(o, "state", t.state);
    t.hasMode = readString(o, "mode", mode) && parseMode(mode, t.mode);
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
    // Поля реальной PS: "temp" — температура платы, "voltages" — напряжения, "cpu_load"/"cpu_cores" — загрузка.
    if (const auto it = o.find("temp"); it != o.end() && it->is_number()) {
        t.temperature.emplace_back("плата", it->get<double>());
    }
    numbers("voltages", t.power);
    if (const auto it = o.find("cpu_load"); it != o.end() && it->is_number()) {
        t.cpuLoad = it->get<double>();
    }
    if (const auto it = o.find("cpu_cores"); it != o.end() && it->is_array()) {
        for (const auto& v : *it) {
            if (v.is_number()) {
                t.cpuCores.push_back(v.get<double>());
            }
        }
    }
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

// -------------------------------------------------------------------- OSD

const std::vector<std::string_view>& osdFieldIds()
{
    static const std::vector<std::string_view> ids{"state", "freq", "az", "cls", "snr"};
    return ids;
}

std::string osdFieldRu(std::string_view id)
{
    static const std::map<std::string_view, std::string_view> names = {
        {"state", "состояние"},
        {"freq", "частота видеоприёмника"},
        {"az", "азимут пеленга"},
        {"cls", "классификация"},
        {"snr", "сигнал/шум"},
    };
    const auto it = names.find(id);
    return it == names.end() ? std::string(id) : std::string(it->second);
}

bool OsdParams::validate(std::string* error) const
{
    if (!en && !standard && !src && !fields && !hos && !vos) {
        setError(error, "OSD: не задан ни один параметр");
        return false;
    }
    if (en && *en != 0 && *en != 1) {
        setError(error, "OSD: en — 0 или 1");
        return false;
    }
    if (standard && *standard != "PAL" && *standard != "NTSC") {
        setError(error, "OSD: std — PAL или NTSC");
        return false;
    }
    if (src && *src != "AUTO" && *src != "TEXT") {
        setError(error, "OSD: src — AUTO или TEXT");
        return false;
    }
    if (fields) {
        const auto& known = osdFieldIds();
        std::set<std::string> seen;
        for (const auto& f : *fields) {
            if (std::find(known.begin(), known.end(), f) == known.end()) {
                setError(error, "OSD: неизвестное поле «" + f + "»");
                return false;
            }
            if (!seen.insert(f).second) {
                setError(error, "OSD: поле «" + f + "» повторяется");
                return false;
            }
        }
    }
    for (const auto& [value, name] : {std::pair{hos, "hos"}, std::pair{vos, "vos"}}) {
        if (value && (*value < kOsdOffsetMin || *value > kOsdOffsetMax)) {
            setError(error, std::string("OSD: ") + name + " — от −32 до +31");
            return false;
        }
    }
    return true;
}

nlohmann::json OsdParams::toJson() const
{
    json p = json::object();
    if (en) p["en"] = *en;
    if (standard) p["std"] = *standard;
    if (src) p["src"] = *src;
    if (fields) p["fields"] = *fields;
    if (hos) p["hos"] = *hos;
    if (vos) p["vos"] = *vos;
    return p;
}

bool OsdParams::fromJson(const nlohmann::json& o, OsdParams& out, std::string* error)
{
    if (!o.is_object()) {
        setError(error, "OSD: payload не объект");
        return false;
    }
    OsdParams p;
    const auto readOptInt = [&o](std::string_view key, std::optional<int>& dst) {
        if (o.find(key) == o.end()) {
            return true;
        }
        std::int64_t v = 0;
        if (!readInt(o, key, v) || v < -1000 || v > 1000) {
            return false;
        }
        dst = static_cast<int>(v);
        return true;
    };
    const auto readOptString = [&o](std::string_view key, std::optional<std::string>& dst) {
        if (o.find(key) == o.end()) {
            return true;
        }
        std::string v;
        if (!readString(o, key, v)) {
            return false;
        }
        dst = std::move(v);
        return true;
    };
    if (!readOptInt("en", p.en) || !readOptInt("hos", p.hos) || !readOptInt("vos", p.vos) ||
        !readOptInt("los", p.los) || !readOptString("std", p.standard) ||
        !readOptString("src", p.src)) {
        setError(error, "OSD: неверный тип параметра");
        return false;
    }
    if (const auto f = o.find("fields"); f != o.end()) {
        if (!f->is_array()) {
            setError(error, "OSD: fields — массив");
            return false;
        }
        std::vector<std::string> ids;
        for (const auto& v : *f) {
            if (!v.is_string()) {
                setError(error, "OSD: fields — массив строк");
                return false;
            }
            ids.push_back(v.get<std::string>());
        }
        p.fields = std::move(ids);
    }
    if (p.los && *p.los != 0 && *p.los != 1) {
        setError(error, "OSD: los — 0 или 1");
        return false;
    }
    if (!p.validate(error)) {
        return false;
    }
    out = std::move(p);
    return true;
}

bool osdTextPayload(int row, int col, std::string_view text, bool ntsc, nlohmann::json& payload,
                    std::string* error)
{
    const int rows = ntsc ? kOsdRowsNtsc : kOsdRowsPal;
    if (row < 0 || row >= rows) {
        setError(error, "текст OSD: строка от 0 до " + std::to_string(rows - 1) +
                            (ntsc ? " (NTSC)" : " (PAL)"));
        return false;
    }
    if (col < 0 || col >= kOsdColumns) {
        setError(error, "текст OSD: столбец от 0 до 29");
        return false;
    }
    if (text.empty()) {
        setError(error, "текст OSD: пустая строка");
        return false;
    }
    if (static_cast<int>(text.size()) + col > kOsdColumns) {
        setError(error, "текст OSD: столбец + длина текста больше 30");
        return false;
    }
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) {
            setError(error, "текст OSD: только латиница, цифры и знаки ASCII 0x20–0x7E");
            return false;
        }
    }
    payload = json{{"row", row}, {"col", col}, {"text", std::string(text)}};
    return true;
}

// ---------------------------------------------------- полное сканирование

bool SffParams::validate(std::string* error) const
{
    if (freq_min < kSffFrequencyMinHz || freq_max > kSffFrequencyMaxHz || freq_min >= freq_max) {
        setError(error, "частоты обзора: нужно 4,9 ГГц ≤ от < до ≤ 6,0 ГГц");
        return false;
    }
    if (az_min < -kAzimuthLimitDeg || az_max > kAzimuthLimitDeg || az_min >= az_max) {
        setError(error, "сектор обзора: нужно −30° ≤ от < до ≤ +30°");
        return false;
    }
    return true;
}

nlohmann::json SffParams::toJson() const
{
    return json{{"freq_min", freq_min}, {"freq_max", freq_max}, {"az_min", az_min},
                {"az_max", az_max}};
}

bool SffParams::fromJson(const nlohmann::json& o, SffParams& out, std::string* error)
{
    SffParams p;
    if (!readInt(o, "freq_min", p.freq_min) || !readInt(o, "freq_max", p.freq_max) ||
        !readNumber(o, "az_min", p.az_min) || !readNumber(o, "az_max", p.az_max)) {
        setError(error, "нет параметров обзора freq_min, freq_max, az_min, az_max");
        return false;
    }
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

namespace {

/// Поля источника, общие для таблиц 23 и 24; без az.
bool readTargetFields(const json& o, OpTarget& t)
{
    return readNumber(o, "w", t.w) && readNumber(o, "snr", t.snr) && readString(o, "cls", t.cls) &&
           readNumber(o, "conf", t.conf) && readNumber(o, "coh", t.coh) && readNumber(o, "df", t.df);
}

}  // namespace

bool OpSurveyResult::fromJson(const nlohmann::json& o, OpSurveyResult& out, std::string* error)
{
    const auto it = o.find("tgt");
    if (it == o.end() || !it->is_array()) {
        setError(error, "send_op_survey_result: нет массива tgt");
        return false;
    }
    OpSurveyResult r;
    for (const auto& item : *it) {
        OpTarget t;
        if (!item.is_object() || !readNumber(item, "az", t.az) || !readTargetFields(item, t)) {
            setError(error, "send_op_survey_result: в элементе tgt нужны az, w, snr, cls, conf, coh, df");
            return false;
        }
        r.tgt.push_back(std::move(t));
    }
    out = std::move(r);
    return true;
}

bool OpTrackResult::fromJson(const nlohmann::json& o, OpTrackResult& out, std::string* error)
{
    OpTrackResult r;
    if (!readString(o, "st", r.st) || !readNumber(o, "az_ray", r.az_ray) || !readNumber(o, "az", r.az) ||
        !readNumber(o, "az_pred", r.az_pred) || !readNumber(o, "az_err", r.az_err) ||
        !readNumber(o, "az_rate", r.az_rate) || !readTargetFields(o, r.target) || !readString(o, "src", r.src) ||
        !readString(o, "meth", r.meth)) {
        setError(error, "send_op_track_result: не хватает полей таблицы 24");
        return false;
    }
    r.target.az = r.az;
    out = std::move(r);
    return true;
}

std::string opClassRu(std::string_view cls)
{
    if (cls == "DRONE") return "БПЛА";
    if (cls == "INTERF") return "помеха";
    if (cls == "UNKNOWN") return "неизвестно";
    return std::string(cls);
}

std::string opTrackStateRu(std::string_view st)
{
    if (st == "ACQUIRE") return "захват";
    if (st == "TRACK") return "сопровождение";
    if (st == "LOSS") return "потеря";
    if (st == "LIMITED_SURVEY") return "ограниченный поиск";
    return std::string(st);
}

}  // namespace afar::ps
