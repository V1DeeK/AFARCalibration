#pragma once

#include <string_view>

namespace afar {

/// Разбор десятичного числа с точкой независимо от локали процесса.
/// QApplication на Linux/macOS вызывает setlocale(LC_ALL, ""), после чего
/// std::stod/strtod в русской локали ждут запятую. Здесь — всегда точка.
/// Вся строка должна быть числом; иначе false и `out` не меняется.
bool parse_double(std::string_view text, double& out) noexcept;

}  // namespace afar
