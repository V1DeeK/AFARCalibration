#pragma once

// Приёмочная проверка сервисного режима PS: перечень шагов и оценка ответа.
// Без сети и без GUI. Контракт: docs/contracts/ps-json-protocol.md, §3–4, §6.

#include "PsProtocol.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace afar::ps {

/// Что проверяет шаг: ответ на команду по TCP или телеметрию по UDP.
enum class CheckKind {
    Command,          ///< отправить msgType и сверить ответ
    TelemetryMode,    ///< дождаться датаграммы с режимом expectMode
    TelemetryClean,   ///< за проверку ни одной потерянной и битой датаграммы
};

/// Сколько ждать датаграмму телеметрии: период PS 1 с (§5) с запасом.
inline constexpr int kTelemetryWaitMs = 3000;

struct CheckStep {
    CheckKind kind = CheckKind::Command;
    Mode expectMode = Mode::Idle;
    std::string phase;     ///< этап: «Связь», «Сервисный режим», …
    std::string title;     ///< что проверяем, по-русски
    std::string msgType;
    nlohmann::json payload = nlohmann::json::object();
    Status expectStatus = Status::Ok;
    /// Поля ответа, которые должны совпасть (числа — с допуском 1e-6).
    nlohmann::json expectFields = nlohmann::json::object();
    /// Ответ должен вернуть ровно те коды 16 каналов, что в expectCodes.
    std::vector<ChannelCodes> expectCodes;
    /// Поля, которые должны быть в ответе с любым значением (temp, lock).
    std::vector<std::string> expectKeys;
    /// Каналы смесителя в ответе: заданные поля должны совпасть.
    std::vector<MixerChannel> expectMixer;
    /// В ответе все 4 канала смесителя (get_mixer).
    bool expectMixerAll = false;
    std::string section;   ///< раздел контракта
    bool proposal = false; ///< шаг проверяет [предложение] контракта
};

enum class CheckVerdict { NotRun, Pass, Fail, NoAnswer };

struct CheckResult {
    CheckVerdict verdict = CheckVerdict::NotRun;
    std::string detail;    ///< почему не прошёл, по-русски
};

/// Шаги проверки: вход в SERVICE из IDLE, все сервисные команды v3 с обратным
/// чтением, отказы на неверные аргументы и вне SERVICE, телеметрия по UDP,
/// возврат в IDLE.
/// Последняя запись кодов — безопасное состояние 63/0 (§7).
std::vector<CheckStep> serviceCheckSteps();

/// Оценка ответа PS на шаг-команду.
CheckResult evaluate(const CheckStep& step, Status status, const nlohmann::json& payload);
/// Оценка датаграммы телеметрии на шаг TelemetryMode.
CheckResult evaluateTelemetry(const CheckStep& step, const Telemetry& t);

std::string verdictRu(CheckVerdict v);
/// Ожидаемый результат шага одной строкой: «OK, src=azimuth, az=12.5».
std::string expectedText(const CheckStep& step);
/// Полученный ответ одной строкой.
std::string responseText(Status status, const nlohmann::json& payload);

}  // namespace afar::ps
