#include "NumberText.h"

#include <cctype>
#include <string>

#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L \
    && !defined(AFAR_NUMBERTEXT_FORCE_STRTOD)
#  define AFAR_NUMBERTEXT_USE_FROM_CHARS 1
#  include <charconv>
#  include <system_error>
#else
#  include <clocale>
#  include <cstdlib>
#  if defined(__APPLE__)
#    include <xlocale.h>
#  else
#    include <locale.h>
#  endif
#endif

namespace afar {

namespace {

#if !defined(AFAR_NUMBERTEXT_USE_FROM_CHARS)
// libc++ Apple Clang не даёт from_chars(double) и не объявляет __cpp_lib_to_chars.
// strtod_l с локалью "C" понимает nan/inf и денормализованные числа так же,
// как from_chars; istringstream их отвергает.
locale_t c_numeric_locale()
{
    static const locale_t loc = ::newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
    return loc;
}

bool parse_with_strtod(const std::string& text, double& out) noexcept
{
    const locale_t loc = c_numeric_locale();
    if (loc == static_cast<locale_t>(0)) {
        return false;
    }
    char* end = nullptr;
    const double value = ::strtod_l(text.c_str(), &end, loc);
    if (end != text.c_str() + text.size()) {
        return false;
    }
    out = value;
    return true;
}
#endif

}  // namespace

bool parse_double(std::string_view text, double& out) noexcept
{
    if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        return false;
    }
    // Лидирующий '+' from_chars не принимает, а stod принимал.
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+') {
            return false;
        }
    }
#if defined(AFAR_NUMBERTEXT_USE_FROM_CHARS)
    double value = 0.0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = value;
    return true;
#else
    try {
        return parse_with_strtod(std::string(text), out);
    } catch (...) {
        return false;
    }
#endif
}

}  // namespace afar
