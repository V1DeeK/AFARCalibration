#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "InverseLut.h"
#include "MeasurementOrchestrator.h"
#include "ParquetExport.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"
#include "qt_test_application.h"
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

TEST_CASE("indexed inverse LUT matches brute-force selection", "[lut_perf][CAL-005]")
{
    std::vector<afar::cal::DirectLutEntry> direct;
    const auto add = [&direct](std::uint16_t att,
                               std::uint8_t phase,
                               double measured_att,
                               double measured_phase,
                               bool valid) {
        afar::cal::DirectLutEntry entry;
        entry.channel = 1;
        entry.freq_hz = 1'296'000'000;
        entry.att_code = att;
        entry.phase_code = phase;
        entry.atten_meas_db = measured_att;
        entry.phase_unwrapped_deg = measured_phase;
        entry.valid = valid;
        direct.push_back(entry);
    };
    add(1, 0, 0.2, 359.0, true);
    add(2, 1, 9.8, 91.0, true);
    add(3, 2, 10.1, 181.0, false);
    add(4, 3, 0.1, -89.0, true);

    afar::RunConfig config;
    config.dut.phase_codes = {0, 3, 90.0};
    config.limits.max_phase_residual_deg = 180.0;
    afar::AttenuatorCodes attenuation;
    attenuation.rows = {{1, 0.0, true, 0}, {2, 10.0, true, 0}};

    std::vector<afar::report::InverseLutEntry> indexed;
    std::string diagnostics;
    REQUIRE(afar::report::buildInverseLutFromDirect(
        direct, config, attenuation, indexed, diagnostics));
    REQUIRE(indexed.size() == 8);

    std::vector<afar::cal::InverseLutCandidate> candidates;
    for (const auto& row : direct) {
        candidates.push_back({row.att_code, row.phase_code, row.atten_meas_db,
                              row.phase_unwrapped_deg, row.valid});
    }
    for (const auto& row : indexed) {
        const auto brute = afar::cal::select_inverse_codes(
            row.target_atten_db, row.target_phase_deg, candidates);
        REQUIRE(brute.found);
        REQUIRE(row.selected_att_code == brute.selected_att_code);
        REQUIRE(row.selected_phase_code == brute.selected_phase_code);
        REQUIRE(row.atten_residual_db == Catch::Approx(brute.atten_residual_db));
        REQUIRE(row.phase_residual_deg == Catch::Approx(brute.phase_residual_deg));
    }
}

TEST_CASE("CAL-005 LUT perf on probe grid (no 10-min gate)",
          "[lut_perf][CAL-005][NFR-03]")
{
    (void)afar::test::guiApplication();
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
    (void)afar::test::guiApplication();
    if (!envAt04Enabled()) {
        SKIP("Set AFAR_RUN_AT04=1 and run with filter [full] for full-volume LUT timing");
    }

    if (const char* existing = std::getenv("AFAR_AT04_SERIES");
        existing != nullptr && existing[0] != '\0') {
        const std::filesystem::path series(existing);
        afar::RunConfig cfg;
        afar::AttenuatorCodes att;
        afar::RawS21Store store;
        std::string diag;
        REQUIRE(afar::RunConfig::loadFromFile(series / "run-config.json", cfg, diag));
        REQUIRE(afar::AttenuatorCodes::loadFromFile(
            series / "attenuator-codes.csv", att, diag));
        REQUIRE(afar::RawS21Store::open(series / "raw-s21.h5", store, diag));
        REQUIRE(store.completedCount() == 65536u);
        REQUIRE(store.totalComplexSamplesCompleted() == 13172736u);
        timeLutBuild(store, cfg, att, 13172736u);
        return;
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
