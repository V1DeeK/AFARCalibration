#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"

#include <filesystem>
#include <string>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("AT-07 VNA timeout retries then error", "[faults][AT-07][TEST-009]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_fault_timeout";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    const std::string run_id = "RX16-20260922-007";
    afar::test::writeProbeFixtures(fixtures, run_id, 5);

    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diag;
    REQUIRE(afar::test::loadProbeConfig(fixtures, cfg, att, diag));

    VnaSimulator vna;
    DutSimulator dut;
    afar::MeasurementOrchestrator orch(&vna, &dut);
    orch.setConfig(cfg, att);
    orch.setSleepEnabled(false);

    REQUIRE(orch.prepare(root / "data", fixtures / "run-config.json",
                         fixtures / "attenuator-codes.csv", diag));
    REQUIRE(orch.start(diag));

    vna.set_failure_mode(VnaSimulator::FailureMode::Timeout);
    orch.runUntilDone();

    REQUIRE(orch.state() == afar::RunState::Error);
    REQUIRE_THAT(orch.lastError(), ContainsSubstring("timeout"));
    // Слот помечен completed с valid=false после 1+2 попыток.
    REQUIRE(orch.store().completedCount() == 1);
    afar::RawS21StateRecord rec;
    REQUIRE(orch.store().readState(1, 0, 0, rec, diag));
    REQUIRE(rec.completed);
    REQUIRE(rec.attempt == 1 + afar::MeasurementOrchestrator::kVnaRetries);
    REQUIRE_FALSE(rec.valid.empty());
    REQUIRE(rec.valid.front() == 0);
}

TEST_CASE("AT-08 DUT reject does not measure", "[faults][AT-08][TEST-009]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_fault_dut";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    const std::string run_id = "RX16-20260922-008";
    afar::test::writeProbeFixtures(fixtures, run_id, 5);

    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diag;
    REQUIRE(afar::test::loadProbeConfig(fixtures, cfg, att, diag));

    VnaSimulator vna;
    DutSimulator dut;
    afar::MeasurementOrchestrator orch(&vna, &dut);
    orch.setConfig(cfg, att);
    orch.setSleepEnabled(false);

    REQUIRE(orch.prepare(root / "data", fixtures / "run-config.json",
                         fixtures / "attenuator-codes.csv", diag));
    REQUIRE(orch.start(diag));

    dut.reject_next_apply();
    orch.runUntilDone();

    REQUIRE(orch.state() == afar::RunState::Error);
    REQUIRE_THAT(orch.lastError(), ContainsSubstring("channel="));
    REQUIRE_THAT(orch.lastError(), ContainsSubstring("att_code="));
    REQUIRE_THAT(orch.lastError(), ContainsSubstring("phase_code="));
    // Без успешного apply слот не completed.
    REQUIRE(orch.store().completedCount() == 0);
}
