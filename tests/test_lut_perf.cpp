#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "ParquetExport.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"
#include "sim_grid_fixtures.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

bool envAt04Enabled()
{
    const char* v = std::getenv("AFAR_RUN_AT04");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

void timeLutBuild(const afar::RawS21Store& store,
                  const afar::RunConfig& cfg,
                  const afar::AttenuatorCodes& att,
                  std::size_t expected_direct_min)
{
    std::string diag;
    std::vector<afar::cal::DirectLutEntry> direct;

    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(afar::report::buildDirectLutFromStore(store, cfg, direct, diag));
    const auto t1 = std::chrono::steady_clock::now();

    REQUIRE(direct.size() >= expected_direct_min);
    REQUIRE_FALSE(direct.empty());

    const auto direct_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    INFO("buildDirectLutFromStore ms=" << direct_ms << " rows=" << direct.size());
    // NFR-03 цель ≤ 10 мин на эталонном ПК — не failing-гейт до эталона.
    std::cout << "[lut_perf] direct LUT: " << direct.size() << " rows in " << direct_ms
              << " ms\n";

    std::vector<afar::report::InverseLutEntry> inverse;
    const auto t2 = std::chrono::steady_clock::now();
    REQUIRE(afar::report::buildInverseLutFromDirect(direct, cfg, att, inverse, diag));
    const auto t3 = std::chrono::steady_clock::now();

    REQUIRE_FALSE(inverse.empty());
    const auto inverse_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    INFO("buildInverseLutFromDirect ms=" << inverse_ms << " rows=" << inverse.size());
    std::cout << "[lut_perf] inverse LUT: " << inverse.size() << " rows in " << inverse_ms
              << " ms\n";

    REQUIRE(afar::report::countValidDirect(direct) > 0);
}

}  // namespace

TEST_CASE("CAL-005 LUT perf on probe grid (no 10-min gate)",
          "[lut_perf][CAL-005][NFR-03]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_lut_perf_probe";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    afar::test::writeProbeFixtures(fixtures, "RX16-20260922-CAL005", 201);

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

    // 1×2×4×201 = 1608 direct rows
    timeLutBuild(orch.store(), cfg, att, 1608);
}

TEST_CASE("CAL-005 LUT perf on full AT-04 volume",
          "[.][full][lut_perf][CAL-005][NFR-03]")
{
    if (!envAt04Enabled()) {
        SKIP("Set AFAR_RUN_AT04=1 and run with filter [full] for full-volume LUT timing");
    }

    const auto root = std::filesystem::temp_directory_path() / "afar_lut_perf_full";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    afar::test::writeSimGridFixtures(fixtures, "RX16-20260922-CAL005-full", 16, 64, 63, 201);

    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diag;
    REQUIRE(afar::test::loadSimGridConfig(fixtures, cfg, att, diag));

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
    REQUIRE(orch.store().completedCount() == 65536u);
    REQUIRE(orch.store().totalComplexSamplesCompleted() == 13172736u);

    timeLutBuild(orch.store(), cfg, att, 13172736u);
}
