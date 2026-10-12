#include "NumberText.h"

#include <charconv>
#include <locale>
#include <sstream>
#include <string>
#include <system_error>

namespace afar {

bool parse_double(std::string_view text, double& out) noexcept
{
    if (text.empty()) {
        return false;
    }
    // Лидирующий '+' from_chars не принимает, а stod принимал.
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+') {
            return false;
        }
    }
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
#else
    // libc++ Apple Clang до 16 без from_chars(double): поток с классической локалью.
    try {
        std::istringstream in{std::string(text)};
        in.imbue(std::locale::classic());
        double value = 0.0;
        in >> value;
        if (in.fail() || in.peek() != std::char_traits<char>::eof()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
#endif
}

}  // namespace afar
