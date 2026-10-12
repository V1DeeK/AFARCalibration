#pragma once

// Обмен с PS изделия: кодирование и разбор сообщений без сети и без GUI.
// Контракт: docs/contracts/ps-json-protocol.md.

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>

#include <string>
#include <string_view>
#include <vector>

namespace afar::ps {

inline constexpr std::string_view kProtocolVersion = "1.0";
inline constexpr std::uint16_t kDefaultControlPort = 49600;
inline constexpr std::uint16_t kDefaultTelemetryPort = 49601;
/// Строка NDJSON длиннее — разрыв соединения (§2).
inline constexpr std::size_t kMaxLineBytes = 1024 * 1024;
/// Датаграмма телеметрии больше — отбрасывается.
inline constexpr std::size_t kMaxDatagramBytes = 65507;

inline constexpr int kChannels = 16;
inline constexpr int kCodeMax = 63;
inline constexpr std::int64_t kFrequencyLimitHz = 100'000'000'000LL;
inline constexpr double kAzimuthLimitDeg = 30.0;

namespace msg {
inline constexpr std::string_view Hello = "hello";
inline constexpr std::string_view KeepAlive = "keep_alive";
inline constexpr std::string_view SetMode = "set_mode";
inline constexpr std::string_view GetState = "get_state";
inline constexpr std::string_view Rs485Send = "rs485_send";
inline constexpr std::string_view SetPhaseAtten = "set_phase_atten";
inline constexpr std::string_view GetPhaseAtten = "get_phase_atten";
inline constexpr std::string_view SetAdc = "set_adc";
inline constexpr std::string_view GetAdc = "get_adc";
inline constexpr std::string_view SetBeamAzimuth = "set_beam_azimuth";
inline constexpr std::string_view SetBeam = "set_beam";
inline constexpr std::string_view GetBeam = "get_beam";
inline constexpr std::string_view Telemetry = "telemetry";
}  // namespace msg

enum class MsgClass { Command, Response, Event };
enum class Status { Ok, Busy, NotSupported, InvalidArgument, HwNotReady };
enum class Mode { Idle, FullScanFreq, Operational, Service };

std::string_view toString(MsgClass v) noexcept;
std::string_view toString(Status v) noexcept;
std::string_view toString(Mode v) noexcept;
bool parseMsgClass(std::string_view s, MsgClass& out) noexcept;
bool parseStatus(std::string_view s, Status& out) noexcept;
bool parseMode(std::string_view s, Mode& out) noexcept;

/// Русские подписи для интерфейса.
std::string statusRu(Status v);
std::string modeRu(Mode v);
std::string stateRu(std::string_view state);

/// Совместимы версии с одинаковой старшей цифрой (§3).
bool versionsCompatible(std::string_view a, std::string_view b);

struct Envelope {
    std::string msg_type;
    MsgClass msg_class = MsgClass::Command;
    std::uint8_t msg_count = 0;
    nlohmann::json payload = nlohmann::json::object();
};

/// Одна строка NDJSON с завершающим '\n'.
std::string encodeLine(const Envelope& env);
/// Датаграмма: тот же JSON без '\n'.
std::string encodeDatagram(const Envelope& env);
/// Разбор строки (с '\n' или без). false и текст в error при нарушении §2.
bool decodeLine(std::string_view line, Envelope& out, std::string* error = nullptr);

Envelope makeCommand(std::string_view msgType, std::uint8_t count,
                     nlohmann::json payload = nlohmann::json::object());
/// Статус ответа; false, если поля нет или значение неизвестно.
bool responseStatus(const Envelope& response, Status& out) noexcept;

/// Разбивка потока TCP на строки с пределом kMaxLineBytes.
class LineSplitter {
public:
    /// false — строка превысила предел, соединение рвать.
    bool append(std::string_view bytes);
    /// Следующая полная строка без '\n'.
    bool takeLine(std::string& line);
    void clear() noexcept { buffer_.clear(); }

private:
    std::string buffer_;
};

// ------------------------------------------------------------------ payload

struct Hello {
    std::string ver{kProtocolVersion};
    std::string tlm_ip;
    std::uint16_t tlm_port = kDefaultTelemetryPort;

    [[nodiscard]] nlohmann::json toJson() const;
};

struct StateInfo {
    Mode mode = Mode::Idle;
    std::string state;

    static bool fromJson(const nlohmann::json& o, StateInfo& out, std::string* error = nullptr);
};

struct ChannelCodes {
    int n = 1;
    int ph = 0;
    int att = 0;

    friend bool operator==(const ChannelCodes&, const ChannelCodes&) = default;
};

/// Ровно 16 каналов, номера 1…16 без повторов, коды 0…63 (§4.1).
/// В out — по возрастанию номера канала.
bool channelsFromJson(const nlohmann::json& payload, std::vector<ChannelCodes>& out,
                      std::string* error = nullptr);
/// Проверка и упорядочение перед отправкой; те же правила, что при разборе.
bool channelsToJson(const std::vector<ChannelCodes>& channels, nlohmann::json& payload,
                    std::string* error = nullptr);

struct AdcParams {
    std::int64_t lo = 0;
    std::int64_t bw = 0;
    std::int64_t fs = 0;
    double gain = 0.0;

    [[nodiscard]] bool validate(std::string* error = nullptr) const;
    [[nodiscard]] nlohmann::json toJson() const;
    static bool fromJson(const nlohmann::json& o, AdcParams& out, std::string* error = nullptr);
};

enum class BeamSource { Azimuth, Table, Codes, None };

struct BeamState {
    BeamSource src = BeamSource::None;
    double az = 0.0;
    int beam = 0;

    static bool fromJson(const nlohmann::json& o, BeamState& out, std::string* error = nullptr);
};

/// Чётная длина, только 0-9 A-F a-f, не пусто (§4.4).
bool hexBytesValid(std::string_view hex) noexcept;

// ------------------------------------------------------------- телеметрия

enum class DeviceHealth { Ok, Fail, NotChecked };
std::string_view toString(DeviceHealth v) noexcept;

struct Telemetry {
    std::int64_t time = 0;
    std::int64_t uptime = 0;
    Mode mode = Mode::Idle;
    std::string state;
    std::vector<std::pair<std::string, DeviceHealth>> devices;
    std::vector<std::pair<std::string, double>> temperature;
    std::vector<std::pair<std::string, double>> power;

    static bool fromJson(const nlohmann::json& o, Telemetry& out, std::string* error = nullptr);
};

/// Русская подпись идентификатора самопроверки ("power", "synth", …).
std::string selfTestRu(std::string_view id);

}  // namespace afar::ps
