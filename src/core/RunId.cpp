#include "RunId.h"

#include <cctype>
#include <cstdio>

namespace afar {
namespace {

bool isAsciiAlphaNum(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isAsciiIdChar(char c)
{
    return isAsciiAlphaNum(c) || c == '-' || c == '_';
}

}  // namespace

RunId::RunId(std::string value)
    : value_(std::move(value))
{
}

std::string RunId::formatCanonical(std::uint16_t year,
                                   std::uint8_t month,
                                   std::uint8_t day,
                                   std::uint16_t sequence)
{
    char buf[32]{};
    std::snprintf(buf,
                  sizeof(buf),
                  "RX16-%04u%02u%02u-%03u",
                  static_cast<unsigned>(year),
                  static_cast<unsigned>(month),
                  static_cast<unsigned>(day),
                  static_cast<unsigned>(sequence % 1000u));
    return std::string(buf);
}

bool RunId::isValidUserId(std::string_view id)
{
    if (id.empty()) {
        return false;
    }
    if (!isAsciiAlphaNum(id.front())) {
        return false;
    }
    for (char c : id) {
        if (!isAsciiIdChar(c)) {
            return false;
        }
    }
    return true;
}

std::optional<RunId> RunId::parseCanonical(std::string_view id)
{
    // RX16-YYYYMMDD-NNN — ровно 17 символов.
    if (id.size() != 17) {
        return std::nullopt;
    }
    if (id.substr(0, 5) != "RX16-") {
        return std::nullopt;
    }
    if (id[13] != '-') {
        return std::nullopt;
    }
    for (std::size_t i = 5; i < 13; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(id[i]))) {
            return std::nullopt;
        }
    }
    for (std::size_t i = 14; i < 17; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(id[i]))) {
            return std::nullopt;
        }
    }

    const unsigned month = static_cast<unsigned>((id[9] - '0') * 10 + (id[10] - '0'));
    const unsigned day = static_cast<unsigned>((id[11] - '0') * 10 + (id[12] - '0'));
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return std::nullopt;
    }

    return RunId(std::string(id));
}

}  // namespace afar
