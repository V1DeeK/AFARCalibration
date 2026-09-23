#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "RawS21Store.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"

#include <filesystem>

TEST_CASE("AT-05 pause/resume does not overwrite completed", "[pause_resume][AT-05][TEST-007]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_pause_resume";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    const std::string run_id = "RX16-20260922-005";
    afar::test::writeProbeFixtures(fixtures, run_id, 11);

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

    REQUIRE(orch.stepOnce());
    REQUIRE(orch.stepOnce());
    REQUIRE(orch.stepOnce());
    REQUIRE(orch.store().completedCount() == 3);

    REQUIRE(orch.pause());
    orch.runUntilDone();
    REQUIRE(orch.state() == afar::RunState::Paused);
    REQUIRE(orch.store().completedCount() == 3);

    // AT-05: completed слот нельзя переписать
    afar::RawS21StateRecord existing;
    REQUIRE(orch.store().readState(1, 0, 0, existing, diag));
    REQUIRE(existing.completed);
    existing.attempt = 99;
    REQUIRE_FALSE(orch.store().writeState(1, 0, 0, existing, diag));

    REQUIRE(orch.resume(diag));
    orch.runUntilDone();
    REQUIRE(orch.state() == afar::RunState::Complete);
    REQUIRE(orch.store().completedCount() == 8);
}
