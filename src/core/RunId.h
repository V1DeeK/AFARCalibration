#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace afar {

/// Идентификатор серии измерений (FR-03, CORE-001).
class RunId {
public:
    /// Шаблон ТЗ: RX16-YYYYMMDD-NNN.
    static std::string formatCanonical(std::uint16_t year,
                                       std::uint8_t month,
                                       std::uint8_t day,
                                       std::uint16_t sequence);

    /// Пользовательский id: латиница, цифры, дефис, подчёркивание;
    /// первый символ — буква или цифра.
    static bool isValidUserId(std::string_view id);

    /// Разбор канонического шаблона RX16-YYYYMMDD-NNN.
    static std::optional<RunId> parseCanonical(std::string_view id);

    [[nodiscard]] const std::string& value() const { return value_; }

private:
    explicit RunId(std::string value);

    std::string value_;
};

}  // namespace afar
