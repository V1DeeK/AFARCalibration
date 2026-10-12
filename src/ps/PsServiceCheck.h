#pragma once

// Приёмочная проверка сервисного режима PS: перечень шагов и оценка ответа.
// Без сети и без GUI. Контракт: docs/contracts/ps-json-protocol.md, §3–4, §6.

#include "PsProtocol.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace afar::ps {

struct CheckStep {
    std::string phase;     ///< этап: «Связь», «Сервисный режим», …
    std::string title;     ///< что проверяем, по-русски
    std::string msgType;
    nlohmann::json payload = nlohmann::json::object();
    Status expectStatus = Status::Ok;
    /// Поля ответа, которые должны совпасть (числа — с допуском 1e-6).
    nlohmann::json expectFields = nlohmann::json::object();
    /// Ответ должен вернуть ровно те коды 16 каналов, что в expectCodes.
    std::vector<ChannelCodes> expectCodes;
    std::string section;   ///< раздел контракта
    bool proposal = false; ///< шаг проверяет [предложение] контракта
};

enum class CheckVerdict { NotRun, Pass, Fail, NoAnswer };

struct CheckResult {
    CheckVerdict verdict = CheckVerdict::NotRun;
    std::string detail;    ///< почему не прошёл, по-русски
};

/// Шаги проверки: вход в SERVICE из IDLE, все сервисные команды с обратным
/// чтением, отказы на неверные аргументы и вне SERVICE, возврат в IDLE.
/// Последняя запись кодов — безопасное состояние 63/0 (§7).
std::vector<CheckStep> serviceCheckSteps();

/// Оценка ответа PS на шаг.
CheckResult evaluate(const CheckStep& step, Status status, const nlohmann::json& payload);

std::string verdictRu(CheckVerdict v);
/// Ожидаемый результат шага одной строкой: «OK, src=azimuth, az=12.5».
std::string expectedText(const CheckStep& step);
/// Полученный ответ одной строкой.
std::string responseText(Status status, const nlohmann::json& payload);

}  // namespace afar::ps
