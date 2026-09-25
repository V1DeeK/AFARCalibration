#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"

#include <filesystem>

TEST_CASE("preview measurement returns all four S-parameters", "[probe_run][sparams]")
{
    VnaSimulator vna;
    DutSimulator dut;
    afar::MeasurementOrchestrator orch(&vna, &dut);
    SweepConfig sweep{};
    sweep.f_start_hz = 1'260'000'000ULL;
    sweep.f_stop_hz = 1'350'000'000ULL;
    sweep.points = 11;
    sweep.ifbw_hz = 10'000;
    sweep.power_dbm = 0.0;
    sweep.averages = 1;

    std::string diagnostics;
    REQUIRE(orch.measurePreview(sweep, diagnostics));
    const auto& result = orch.lastMeasuredSweep();
    REQUIRE(result.frequency_hz.size() == sweep.points);
    REQUIRE(result.s11.size() == sweep.points);
    REQUIRE(result.s21.size() == sweep.points);
    REQUIRE(result.s12.size() == sweep.points);
    REQUIRE(result.s22.size() == sweep.points);
}

TEST_CASE("AT-03 probe run: 1ch x 2att x 4phase x 201 pts", "[probe_run][AT-03][TEST-006]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_probe_run";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    const std::string run_id = "RX16-20260922-003";
    afar::test::writeProbeFixtures(fixtures, run_id, 201);

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
    orch.runUntilDone();

    REQUIRE(orch.state() == afar::RunState::Complete);
    REQUIRE(orch.store().completedCount() == 8);
    REQUIRE(orch.store().totalComplexSamplesCompleted() == 1608);
    REQUIRE(orch.scanOrder().size() == 8);

    const auto& series = orch.series();
    REQUIRE(std::filesystem::is_regular_file(series.directLutPath()));
    REQUIRE(std::filesystem::is_regular_file(series.inverseLutPath()));
    REQUIRE(std::filesystem::is_regular_file(series.reportPath()));
    REQUIRE(std::filesystem::is_regular_file(series.manifestPath()));
}
