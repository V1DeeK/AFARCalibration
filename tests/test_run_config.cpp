#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using Catch::Approx;

#include "AttenuatorCodes.h"
#include "RunConfig.h"
#include "RunId.h"

#include <filesystem>
#include <string>

using Catch::Matchers::ContainsSubstring;

namespace {

std::filesystem::path examplesDir()
{
#ifdef AFAR_EXAMPLES_DIR
    return std::filesystem::path(AFAR_EXAMPLES_DIR);
#else
    return std::filesystem::path("examples");
#endif
}

}  // namespace

TEST_CASE("AT-02: valid example run-config loads without hardware", "[run_config][AT-02]")
{
    afar::RunConfig cfg;
    std::string diag;
    const auto path = examplesDir() / "run-config.example.json";
    REQUIRE(afar::RunConfig::loadFromFile(path, cfg, diag));
    CHECK(diag.empty());
    CHECK(cfg.schema == afar::RunConfig::kSchemaName);
    CHECK(cfg.run_id == "RX16-20260829-001");
    CHECK(cfg.vna.f_start_hz < cfg.vna.f_stop_hz);
    CHECK(cfg.vna.points == 201);
    CHECK(cfg.vna.power_dbm == Approx(-30.0));
    CHECK(cfg.dut.channels.first == 1);
    CHECK(cfg.dut.channels.last == 16);
    CHECK(cfg.dut.phase_codes.first == 0);
    CHECK(cfg.dut.phase_codes.last == 63);
    // FR-02 / AT-02: parse succeeds with no IVna / IDutController involvement.
}

TEST_CASE("AT-02: valid attenuator-codes example", "[run_config][AT-02]")
{
    afar::AttenuatorCodes codes;
    std::string diag;
    const auto path = examplesDir() / "attenuator-codes.csv";
    REQUIRE(afar::AttenuatorCodes::loadFromFile(path, codes, diag));
    CHECK(diag.empty());
    CHECK(codes.rows.size() == 64);
    CHECK(codes.enabledCount() == 64);
    CHECK(codes.rows.back().att_code == 63);
    CHECK(codes.rows.back().settle_ms == 30);
}

TEST_CASE("AT-02: bad frequency range fails with diagnostics", "[run_config][AT-02]")
{
    constexpr std::string_view bad = R"({
  "schema": "afar.stage1.run-config/v1",
  "run_id": "RX16-20260829-001",
  "vna": {
    "model": "PLANAR C2220",
    "host": "127.0.0.1",
    "port": 5025,
    "s_parameter": "S21",
    "f_start_hz": 6000000000,
    "f_stop_hz": 4900000000,
    "points": 201,
    "ifbw_hz": 1000,
    "power_dbm": -30.0,
    "averages": 8
  },
  "controller": { "driver": "serial-v1", "endpoint": "COM7" },
  "dut": {
    "serial": "EXAMPLE",
    "channels": { "first": 1, "last": 16 },
    "phase_codes": { "first": 0, "last": 63, "lsb_deg": 5.625 },
    "attenuator_codes_file": "attenuator-codes.csv",
    "reference": { "att_code": 0, "phase_code": 0 }
  },
  "timing": { "settle_ms": 20, "reference_after_phase_row": true },
  "limits": { "max_drift_phase_deg": 1.0, "max_phase_residual_deg": 2.8125 }
})";

    afar::RunConfig cfg;
    std::string diag;
    REQUIRE_FALSE(afar::RunConfig::parse(bad, cfg, diag));
    REQUIRE_THAT(diag, ContainsSubstring("f_start_hz"));
    REQUIRE_THAT(diag, ContainsSubstring("f_stop_hz"));
}

TEST_CASE("AT-02: unknown root field fails", "[run_config][AT-02]")
{
    constexpr std::string_view bad = R"({
  "schema": "afar.stage1.run-config/v1",
  "run_id": "RX16-20260829-001",
  "extra_field": true,
  "vna": {
    "model": "PLANAR C2220",
    "host": "127.0.0.1",
    "port": 5025,
    "s_parameter": "S21",
    "f_start_hz": 4900000000,
    "f_stop_hz": 6000000000,
    "points": 201,
    "ifbw_hz": 1000,
    "power_dbm": -30.0,
    "averages": 8
  },
  "controller": { "driver": "serial-v1", "endpoint": "COM7" },
  "dut": {
    "serial": "EXAMPLE",
    "channels": { "first": 1, "last": 16 },
    "phase_codes": { "first": 0, "last": 63, "lsb_deg": 5.625 },
    "attenuator_codes_file": "attenuator-codes.csv",
    "reference": { "att_code": 0, "phase_code": 0 }
  },
  "timing": { "settle_ms": 20, "reference_after_phase_row": true },
  "limits": { "max_drift_phase_deg": 1.0, "max_phase_residual_deg": 2.8125 }
})";

    afar::RunConfig cfg;
    std::string diag;
    REQUIRE_FALSE(afar::RunConfig::parse(bad, cfg, diag));
    REQUIRE_THAT(diag, ContainsSubstring("extra_field"));
    REQUIRE_THAT(diag, ContainsSubstring("additionalProperties"));
}

TEST_CASE("AT-02: duplicate att_code fails", "[run_config][AT-02]")
{
    constexpr std::string_view bad =
        "att_code,att_cmd_db,enabled,settle_ms\n"
        "0,0.00,true,20\n"
        "1,0.50,true,20\n"
        "0,1.00,true,20\n";

    afar::AttenuatorCodes codes;
    std::string diag;
    REQUIRE_FALSE(afar::AttenuatorCodes::parse(bad, codes, diag));
    REQUIRE_THAT(diag, ContainsSubstring("duplicate"));
    REQUIRE_THAT(diag, ContainsSubstring("att_code"));
}

TEST_CASE("AT-02: power out of instrument range fails", "[run_config][AT-02]")
{
    constexpr std::string_view bad = R"({
  "schema": "afar.stage1.run-config/v1",
  "run_id": "RX16-20260829-001",
  "vna": {
    "model": "PLANAR C2220",
    "host": "127.0.0.1",
    "port": 5025,
    "s_parameter": "S21",
    "f_start_hz": 4900000000,
    "f_stop_hz": 6000000000,
    "points": 201,
    "ifbw_hz": 1000,
    "power_dbm": 15.0,
    "averages": 8
  },
  "controller": { "driver": "serial-v1", "endpoint": "COM7" },
  "dut": {
    "serial": "EXAMPLE",
    "channels": { "first": 1, "last": 16 },
    "phase_codes": { "first": 0, "last": 63, "lsb_deg": 5.625 },
    "attenuator_codes_file": "attenuator-codes.csv",
    "reference": { "att_code": 0, "phase_code": 0 }
  },
  "timing": { "settle_ms": 20, "reference_after_phase_row": true },
  "limits": { "max_drift_phase_deg": 1.0, "max_phase_residual_deg": 2.8125 }
})";

    afar::RunConfig cfg;
    std::string diag;
    REQUIRE_FALSE(afar::RunConfig::parse(bad, cfg, diag));
    REQUIRE_THAT(diag, ContainsSubstring("power_dbm"));
}

TEST_CASE("CORE-001: RunId canonical template and user id", "[run_config][run_id]")
{
    CHECK(afar::RunId::formatCanonical(2026, 8, 29, 1) == "RX16-20260829-001");
    CHECK(afar::RunId::isValidUserId("RX16-20260829-001"));
    CHECK(afar::RunId::isValidUserId("My_Run-01"));
    CHECK_FALSE(afar::RunId::isValidUserId(""));
    CHECK_FALSE(afar::RunId::isValidUserId("-bad"));
    CHECK_FALSE(afar::RunId::isValidUserId("bad id"));
    CHECK_FALSE(afar::RunId::isValidUserId("кириллица"));

    auto parsed = afar::RunId::parseCanonical("RX16-20260829-001");
    REQUIRE(parsed.has_value());
    CHECK(parsed->value() == "RX16-20260829-001");
    CHECK_FALSE(afar::RunId::parseCanonical("RX16-20260829-1").has_value());
}

TEST_CASE("AT-02: invalid run_id fails", "[run_config][AT-02]")
{
    constexpr std::string_view bad = R"({
  "schema": "afar.stage1.run-config/v1",
  "run_id": "bad id!",
  "vna": {
    "model": "PLANAR C2220",
    "host": "127.0.0.1",
    "port": 5025,
    "s_parameter": "S21",
    "f_start_hz": 4900000000,
    "f_stop_hz": 6000000000,
    "points": 201,
    "ifbw_hz": 1000,
    "power_dbm": -30.0,
    "averages": 8
  },
  "controller": { "driver": "serial-v1", "endpoint": "COM7" },
  "dut": {
    "serial": "EXAMPLE",
    "channels": { "first": 1, "last": 16 },
    "phase_codes": { "first": 0, "last": 63, "lsb_deg": 5.625 },
    "attenuator_codes_file": "attenuator-codes.csv",
    "reference": { "att_code": 0, "phase_code": 0 }
  },
  "timing": { "settle_ms": 20, "reference_after_phase_row": true },
  "limits": { "max_drift_phase_deg": 1.0, "max_phase_residual_deg": 2.8125 }
})";

    afar::RunConfig cfg;
    std::string diag;
    REQUIRE_FALSE(afar::RunConfig::parse(bad, cfg, diag));
    REQUIRE_THAT(diag, ContainsSubstring("run_id"));
}

TEST_CASE("DATA-101: s_parameter accepts S11..S22", "[run_config][DATA-101]")
{
    SParameter p{};
    CHECK(afar::parseSParameter("S11", p));
    CHECK(p == SParameter::S11);
    CHECK(afar::parseSParameter("S12", p));
    CHECK(p == SParameter::S12);
    CHECK(afar::parseSParameter("S21", p));
    CHECK(p == SParameter::S21);
    CHECK(afar::parseSParameter("S22", p));
    CHECK(p == SParameter::S22);
    CHECK_FALSE(afar::parseSParameter("S33", p));
    CHECK_FALSE(afar::parseSParameter("s21", p));

    constexpr std::string_view with_s11 = R"({
  "schema": "afar.stage1.run-config/v1",
  "run_id": "RX16-20260924-011",
  "vna": {
    "model": "PLANAR C2220",
    "host": "127.0.0.1",
    "port": 5025,
    "s_parameter": "S11",
    "f_start_hz": 1246000000,
    "f_stop_hz": 1346000000,
    "points": 101,
    "ifbw_hz": 1000,
    "power_dbm": -30.0,
    "averages": 8
  },
  "controller": { "driver": "serial-v1", "endpoint": "COM7" },
  "dut": {
    "serial": "EXAMPLE",
    "channels": { "first": 1, "last": 16 },
    "phase_codes": { "first": 0, "last": 63, "lsb_deg": 5.625 },
    "attenuator_codes_file": "attenuator-codes.csv",
    "reference": { "att_code": 0, "phase_code": 0 }
  },
  "timing": { "settle_ms": 20, "reference_after_phase_row": true },
  "limits": { "max_drift_phase_deg": 1.0, "max_phase_residual_deg": 2.8125 }
})";

    afar::RunConfig cfg;
    std::string diag;
    REQUIRE(afar::RunConfig::parse(with_s11, cfg, diag));
    CHECK(cfg.vna.s_parameter == "S11");

    constexpr std::string_view bad_sp = R"({
  "schema": "afar.stage1.run-config/v1",
  "run_id": "RX16-20260924-012",
  "vna": {
    "model": "PLANAR C2220",
    "host": "127.0.0.1",
    "port": 5025,
    "s_parameter": "S33",
    "f_start_hz": 1246000000,
    "f_stop_hz": 1346000000,
    "points": 101,
    "ifbw_hz": 1000,
    "power_dbm": -30.0,
    "averages": 8
  },
  "controller": { "driver": "serial-v1", "endpoint": "COM7" },
  "dut": {
    "serial": "EXAMPLE",
    "channels": { "first": 1, "last": 16 },
    "phase_codes": { "first": 0, "last": 63, "lsb_deg": 5.625 },
    "attenuator_codes_file": "attenuator-codes.csv",
    "reference": { "att_code": 0, "phase_code": 0 }
  },
  "timing": { "settle_ms": 20, "reference_after_phase_row": true },
  "limits": { "max_drift_phase_deg": 1.0, "max_phase_residual_deg": 2.8125 }
})";
    REQUIRE_FALSE(afar::RunConfig::parse(bad_sp, cfg, diag));
    REQUIRE_THAT(diag, ContainsSubstring("s_parameter"));
}

TEST_CASE("DATA-101: c2220-1296 example loads", "[run_config][DATA-101]")
{
    afar::RunConfig cfg;
    std::string diag;
    const auto path = examplesDir() / "run-config.c2220-1296.example.json";
    REQUIRE(afar::RunConfig::loadFromFile(path, cfg, diag));
    CHECK(cfg.vna.f_start_hz == 1246000000ULL);
    CHECK(cfg.vna.f_stop_hz == 1346000000ULL);
    CHECK(cfg.vna.points == 101);
    CHECK(cfg.vna.ifbw_hz == 1000);
    CHECK(cfg.vna.s_parameter == "S21");
}

