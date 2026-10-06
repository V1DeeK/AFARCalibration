#pragma once

#include <string>
#include <vector>

/// Поля типичного SCPI `*IDN?`: manufacturer,model,serial,firmware (CSV по запятым).
struct ScpiIdnFields {
    std::string manufacturer;
    std::string model;
    std::string serial;
    std::string firmware;
    /// Исходная строка без хвостовых CR/LF/пробелов.
    std::string raw;
};

inline std::string afar_trim_idn_token(std::string s)
{
    while (!s.empty()
           && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.erase(s.begin());
    }
    while (!s.empty()
           && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

/// Разбор `*IDN?`. Полей меньше четырёх — оставшиеся пустые; лишние склеиваются в firmware.
inline ScpiIdnFields parse_scpi_idn(const std::string& idn)
{
    ScpiIdnFields out;
    out.raw = afar_trim_idn_token(idn);
    std::vector<std::string> parts;
    std::string cur;
    for (const char ch : out.raw) {
        if (ch == ',') {
            parts.push_back(afar_trim_idn_token(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    parts.push_back(afar_trim_idn_token(cur));
    if (!parts.empty()) {
        out.manufacturer = parts[0];
    }
    if (parts.size() > 1) {
        out.model = parts[1];
    }
    if (parts.size() > 2) {
        out.serial = parts[2];
    }
    if (parts.size() > 3) {
        out.firmware = parts[3];
        for (std::size_t i = 4; i < parts.size(); ++i) {
            out.firmware += ',';
            out.firmware += parts[i];
        }
    }
    return out;
}
