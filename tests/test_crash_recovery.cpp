#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"

#include <filesystem>

TEST_CASE("AT-06 crash recovery continues from incomplete", "[crash_recovery][AT-06][TEST-008]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_crash_recovery";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    const std::string run_id = "RX16-20260922-006";
    afar::test::writeProbeFixtures(fixtures, run_id, 11);

    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diag;
    REQUIRE(afar::test::loadProbeConfig(fixtures, cfg, att, diag));

    std::filesystem::path series_path;
    {
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
        REQUIRE(orch.store().completedCount() == 2);
        series_path = orch.series().root();
        // «Краш»: закрываем оркестратор/store без Complete.
    }

    VnaSimulator vna2;
    DutSimulator dut2;
    afar::MeasurementOrchestrator orch2(&vna2, &dut2);
    orch2.setSleepEnabled(false);
    REQUIRE(orch2.prepareRecovery(series_path, diag));
    REQUIRE(orch2.store().completedCount() == 2);
    REQUIRE(orch2.start(diag));
    orch2.runUntilDone();
    REQUIRE(orch2.state() == afar::RunState::Complete);
    REQUIRE(orch2.store().completedCount() == 8);
}
