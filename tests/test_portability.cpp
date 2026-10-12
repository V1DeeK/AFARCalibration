#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "NumberText.h"
#include "ProcessMetrics.h"

#include <clocale>
#include <string>

using Catch::Approx;

namespace {

/// Включает локаль с запятой как десятичным разделителем, если она есть в ОС.
class CommaLocale {
public:
    CommaLocale()
    {
        const char* current = std::setlocale(LC_NUMERIC, nullptr);
        saved_ = current != nullptr ? current : "C";
        for (const char* name : {"ru_RU.UTF-8", "ru_RU.utf8", "de_DE.UTF-8", "de_DE.utf8",
                                 "Russian_Russia.1251", "German_Germany.1252"}) {
            if (std::setlocale(LC_NUMERIC, name) != nullptr) {
                active_ = true;
                return;
            }
        }
    }
    ~CommaLocale() { std::setlocale(LC_NUMERIC, saved_.c_str()); }
    bool active() const noexcept { return active_; }

private:
    std::string saved_;
    bool active_{false};
};

}  // namespace

TEST_CASE("parse_double accepts SCPI and file numbers with a dot", "[portability]")
{
    double v = 0.0;
    REQUIRE(afar::parse_double("1.5", v));
    REQUIRE(v == Approx(1.5));
    REQUIRE(afar::parse_double("-4.9E+09", v));
    REQUIRE(v == Approx(-4.9e9));
    REQUIRE(afar::parse_double("+2.5e-3", v));
    REQUIRE(v == Approx(2.5e-3));
    REQUIRE(afar::parse_double("0", v));
    REQUIRE(v == Approx(0.0));
}

TEST_CASE("parse_double rejects partial and malformed numbers", "[portability]")
{
    double v = 7.0;
    REQUIRE_FALSE(afar::parse_double("", v));
    REQUIRE_FALSE(afar::parse_double("1,5", v));
    REQUIRE_FALSE(afar::parse_double("1.5abc", v));
    REQUIRE_FALSE(afar::parse_double("abc", v));
    REQUIRE_FALSE(afar::parse_double("+-1", v));
    REQUIRE(v == Approx(7.0));
}

TEST_CASE("parse_double ignores a comma-decimal process locale", "[portability]")
{
    CommaLocale locale;
    if (!locale.active()) {
        SKIP("в ОС нет локали с запятой (ru_RU/de_DE)");
    }
    double v = 0.0;
    REQUIRE(afar::parse_double("5.625", v));
    REQUIRE(v == Approx(5.625));
    REQUIRE_FALSE(afar::parse_double("5,625", v));
}

TEST_CASE("process metrics report resident memory and threads", "[portability]")
{
    const auto metrics = afar::current_process_metrics();
    REQUIRE(metrics.rss_bytes > 0);
    REQUIRE(metrics.thread_count >= 1);
    REQUIRE(metrics.handle_count >= 1);
}
