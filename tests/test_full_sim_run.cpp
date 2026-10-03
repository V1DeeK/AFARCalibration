#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "Manifest.h"
#include "MeasurementOrchestrator.h"
#include "ParquetExport.h"
#include "RunReportPdf.h"
#include "VnaSimulator.h"
#include "sim_grid_fixtures.h"
#include "qt_test_application.h"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <tuple>

namespace {

bool envAt04Enabled()
{
    const char* v = std::getenv("AFAR_RUN_AT04");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}

void runSimSeries(const std::filesystem::path& root,
                  const std::string& run_id,
                  int channel_last,
                  int att_count,
                  int phase_last,
                  int points,
                  std::size_t expected_states,
                  std::size_t expected_complex)
{
    (void)afar::test::guiApplication();
    const auto fixtures = root / "fixtures";
    afar::test::writeSimGridFixtures(fixtures, run_id, channel_last, att_count, phase_last,
                                     points);

    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diag;
    REQUIRE(afar::test::loadSimGridConfig(fixtures, cfg, att, diag));
    REQUIRE(cfg.timing.settle_ms == 0);

    VnaSimulator vna;
    DutSimulator dut;
    afar::MeasurementOrchestrator orch(&vna, &dut);
    orch.setConfig(cfg, att);
    orch.setSleepEnabled(false);

    REQUIRE(orch.prepare(root / "data", fixtures / "run-config.json",
                         fixtures / "attenuator-codes.csv", diag));

    REQUIRE(orch.store().nChannel() == static_cast<std::uint32_t>(channel_last));
    REQUIRE(orch.store().nAtt() == static_cast<std::uint32_t>(att_count));
    REQUIRE(orch.store().nPhase() == static_cast<std::uint32_t>(phase_last + 1));
    REQUIRE(orch.store().nFreq() == static_cast<std::uint32_t>(points));
    REQUIRE(orch.scanOrder().size() == expected_states);

    REQUIRE(orch.start(diag));
    orch.runUntilDone();

    REQUIRE(orch.state() == afar::RunState::Complete);
    REQUIRE(orch.store().completedCount() == expected_states);
    REQUIRE(orch.store().totalComplexSamplesCompleted() == expected_complex);

    // Нет пропусков и дублей completed: каждый пункт скана ровно один раз.
    for (std::size_t i = 0; i < orch.scanOrder().size(); ++i) {
        const auto& st = orch.scanOrder().at(i).state;
        REQUIRE(orch.store().isCompleted(st.channel, st.att_code, st.phase_code));
    }
}

}  // namespace

TEST_CASE("AT-04 compact full-grid stress: 16ch x 4att x 4phase x 11 pts",
          "[full_sim_run][AT-04][TEST-012]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_full_sim_compact";
    std::filesystem::remove_all(root);

    constexpr int kCh = 16;
    constexpr int kAtt = 4;
    constexpr int kPhaseLast = 3;  // 0..3 → 4 phases
    constexpr int kPoints = 11;
    constexpr std::size_t kStates = 16u * 4u * 4u;  // 256
    constexpr std::size_t kComplex = kStates * 11u;  // 2816

    runSimSeries(root, "RX16-20260922-AT04-compact", kCh, kAtt, kPhaseLast, kPoints, kStates,
                 kComplex);
}

TEST_CASE("stage 7 demo acceptance: channel 7, 64x64, recovery and exports",
          "[stage7][demo_64x64][acceptance]")
{
    (void)afar::test::guiApplication();
    const auto root = std::filesystem::temp_directory_path() / "afar_stage7_demo_64x64";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    const std::string run_id = "RX16-20261003-STAGE7";
    afar::test::writeSimGridFixtures(fixtures, run_id, 7, 64, 63, 3, 7);

    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diag;
    REQUIRE(afar::test::loadSimGridConfig(fixtures, cfg, att, diag));
    REQUIRE(cfg.dut.channels.first == 7);
    REQUIRE(cfg.dut.channels.last == 7);

    constexpr std::size_t expectedStates = 64u * 64u;
    constexpr std::size_t interruptAfter = 257;
    std::filesystem::path seriesPath;
    afar::RawS21StateRecord preserved;
    DutState preservedState;
    {
        VnaSimulator vna;
        DutSimulator dut;
        vna.set_dut_state_provider([&dut] { return dut.current_state(); });
        afar::MeasurementOrchestrator orch(&vna, &dut);
        orch.setConfig(cfg, att);
        orch.setSleepEnabled(false);
        REQUIRE(orch.prepare(root / "data", fixtures / "run-config.json",
                             fixtures / "attenuator-codes.csv", diag));
        REQUIRE(orch.scanOrder().size() == expectedStates);

        std::set<std::tuple<unsigned, unsigned, unsigned>> unique;
        for (const auto& item : orch.scanOrder().items()) {
            unique.emplace(item.state.channel, item.state.att_code, item.state.phase_code);
        }
        REQUIRE(unique.size() == expectedStates);

        REQUIRE(orch.start(diag));
        for (std::size_t i = 0; i < interruptAfter; ++i) {
            REQUIRE(orch.stepOnce());
        }
        REQUIRE(orch.store().completedCount() == interruptAfter);
        preservedState = orch.scanOrder().at(0).state;
        REQUIRE(orch.store().readState(preservedState.channel, preservedState.att_code,
                                       preservedState.phase_code, preserved, diag));
        REQUIRE(preserved.completed);
        seriesPath = orch.series().root();
    }

    VnaSimulator recoveredVna;
    DutSimulator recoveredDut;
    recoveredVna.set_dut_state_provider([&recoveredDut] {
        return recoveredDut.current_state();
    });
    afar::MeasurementOrchestrator recovered(&recoveredVna, &recoveredDut);
    recovered.setSleepEnabled(false);
    REQUIRE(recovered.prepareRecovery(seriesPath, diag));
    REQUIRE(recovered.store().completedCount() == interruptAfter);
    REQUIRE(recovered.start(diag));
    recovered.runUntilDone();
    REQUIRE(recovered.state() == afar::RunState::Complete);
    REQUIRE(recovered.store().completedCount() == expectedStates);

    afar::RawS21StateRecord retained;
    REQUIRE(recovered.store().readState(preservedState.channel, preservedState.att_code,
                                        preservedState.phase_code, retained, diag));
    REQUIRE(retained.s21 == preserved.s21);
    REQUIRE(retained.attempt == preserved.attempt);

    for (const auto& item : recovered.scanOrder().items()) {
        afar::RawS21StateRecord row;
        REQUIRE(recovered.store().readState(item.state.channel, item.state.att_code,
                                            item.state.phase_code, row, diag));
        REQUIRE(row.completed);
        REQUIRE(row.attempt == 1);
    }

    afar::RawS21StateRecord att0Phase0;
    afar::RawS21StateRecord att1Phase0;
    afar::RawS21StateRecord att0Phase1;
    REQUIRE(recovered.store().readState(7, 0, 0, att0Phase0, diag));
    REQUIRE(recovered.store().readState(7, 1, 0, att1Phase0, diag));
    REQUIRE(recovered.store().readState(7, 0, 1, att0Phase1, diag));
    REQUIRE(att0Phase0.s21 != att1Phase0.s21);
    REQUIRE(att0Phase0.s21 != att0Phase1.s21);

    const auto& series = recovered.series();
    const auto channelParquet = series.root() / "channel-07-calibration.parquet";
    const auto channelCsv = series.root() / "channel-07-calibration.csv";
    const auto channelPdf = series.root() / "channel-07-report.pdf";
    REQUIRE(std::filesystem::is_regular_file(series.rawS21Path()));
    REQUIRE(std::filesystem::is_regular_file(series.directLutPath()));
    REQUIRE(std::filesystem::is_regular_file(series.inverseLutPath()));
    REQUIRE(std::filesystem::is_regular_file(channelParquet));
    REQUIRE(std::filesystem::is_regular_file(channelCsv));
    REQUIRE(std::filesystem::is_regular_file(channelPdf));

    std::vector<afar::cal::DirectLutEntry> direct;
    std::vector<afar::report::InverseLutEntry> inverse;
    std::vector<afar::report::InverseLutEntry> channelInverse;
    std::vector<afar::report::InverseLutEntry> channelCsvRows;
    std::string csvRunId;
    REQUIRE(afar::report::readDirectLut(series.directLutPath(), direct, diag));
    REQUIRE(afar::report::readInverseLut(series.inverseLutPath(), inverse, diag));
    REQUIRE(afar::report::readInverseLut(channelParquet, channelInverse, diag));
    REQUIRE(afar::report::readInverseLutCsv(channelCsv, csvRunId, channelCsvRows, diag));
    REQUIRE(direct.size() == expectedStates * 3u);
    REQUIRE(inverse.size() == expectedStates * 3u);
    REQUIRE(channelInverse.size() == inverse.size());
    REQUIRE(channelCsvRows.size() == channelInverse.size());
    REQUIRE(csvRunId == run_id);
    REQUIRE(afar::report::isValidPdfSmoke(channelPdf, diag));

    recovered.store().close();
    REQUIRE(afar::report::verifyManifest(series, diag));
    std::vector<afar::report::ManifestEntry> manifest;
    REQUIRE(afar::report::readManifestFile(series.manifestPath(), manifest, diag));
    std::set<std::string> names;
    for (const auto& entry : manifest) {
        names.insert(entry.filename);
    }
    for (const auto* required : {"raw-s21.h5", "direct-lut.parquet", "inverse-lut.parquet",
                                 "channel-07-calibration.parquet", "channel-07-calibration.csv",
                                 "channel-07-report.pdf"}) {
        REQUIRE(names.contains(required));
    }
}

TEST_CASE("AT-04 full sim: 16ch x 64att x 64phase x 201 pts",
          "[.][full][full_sim_run][AT-04][TEST-012]")
{
    if (!envAt04Enabled()) {
        SKIP("Set AFAR_RUN_AT04=1 and run with filter [full] for 65536-state AT-04");
    }

    const auto root = std::filesystem::temp_directory_path() / "afar_full_sim_run";
    std::filesystem::remove_all(root);

    constexpr int kCh = 16;
    constexpr int kAtt = 64;
    constexpr int kPhaseLast = 63;  // 0..63 → 64 phases
    constexpr int kPoints = 201;
    constexpr std::size_t kStates = 16u * 64u * 64u;           // 65536
    constexpr std::size_t kComplex = kStates * 201u;            // 13172736

    runSimSeries(root, "RX16-20260922-AT04-full", kCh, kAtt, kPhaseLast, kPoints, kStates,
                 kComplex);
}
