#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"
#include "sim_grid_fixtures.h"

#include <cstdlib>
#include <filesystem>

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
