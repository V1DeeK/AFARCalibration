#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"
#include "qt_test_application.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {

bool enabled()
{
    const char* value = std::getenv("AFAR_RUN_AT12");
    return value != nullptr && value[0] != '\0' && std::string(value) != "0";
}

std::uint64_t envSeconds(const char* name, std::uint64_t fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    try {
        return std::stoull(value);
    } catch (...) {
        return fallback;
    }
}

std::uint64_t residentBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
    std::ifstream statm("/proc/self/statm");
    std::uint64_t total_pages = 0;
    std::uint64_t resident_pages = 0;
    statm >> total_pages >> resident_pages;
    (void)total_pages;
    return resident_pages * static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
#endif
}

}  // namespace

TEST_CASE("AT-12 simulator soak with memory telemetry", "[AT-12][soak]")
{
    (void)afar::test::guiApplication();
    if (!enabled()) {
        SUCCEED("Set AFAR_RUN_AT12=1; default duration is 24 hours");
        return;
    }

    const auto duration_s = envSeconds("AFAR_AT12_SECONDS", 24u * 60u * 60u);
    const auto stabilization_s = envSeconds("AFAR_AT12_STABILIZE_SECONDS", 60u * 60u);
    REQUIRE(duration_s > 0);
    REQUIRE(stabilization_s < duration_s);

    const auto root = std::filesystem::temp_directory_path() / "afar_at12_soak";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto fixtures = root / "fixtures";
    afar::test::writeProbeFixtures(fixtures, "RX16-AT12-TEMPLATE", 11);

    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diagnostics;
    REQUIRE(afar::test::loadProbeConfig(fixtures, cfg, att, diagnostics));

    const auto telemetry_path = root / "at12-soak.csv";
    std::ofstream telemetry(telemetry_path, std::ios::binary | std::ios::trunc);
    REQUIRE(telemetry);
    telemetry << "elapsed_s,iteration,rss_bytes\n";

    const auto started = std::chrono::steady_clock::now();
    std::uint64_t baseline_rss = 0;
    std::uint64_t final_rss = 0;
    std::size_t iterations = 0;
    while (true) {
        const auto elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
                                                             - started)
                .count());
        if (elapsed >= duration_s) {
            break;
        }

        const auto iteration_root = root / ("iteration-" + std::to_string(iterations));
        {
            VnaSimulator vna;
            DutSimulator dut;
            afar::MeasurementOrchestrator orchestrator(&vna, &dut);
            orchestrator.setConfig(cfg, att);
            orchestrator.setSleepEnabled(false);
            REQUIRE(orchestrator.prepare(iteration_root, fixtures / "run-config.json",
                                         fixtures / "attenuator-codes.csv", diagnostics));
            REQUIRE(orchestrator.start(diagnostics));
            orchestrator.runUntilDone();
            REQUIRE(orchestrator.state() == afar::RunState::Complete);
        }
        std::filesystem::remove_all(iteration_root);
        ++iterations;

        const auto sample_elapsed = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now()
                                                             - started)
                .count());
        final_rss = residentBytes();
        telemetry << sample_elapsed << ',' << iterations << ',' << final_rss << '\n';
        telemetry.flush();
        if (baseline_rss == 0 && sample_elapsed >= stabilization_s) {
            baseline_rss = final_rss;
        }
    }

    REQUIRE(iterations > 0);
    REQUIRE(baseline_rss > 0);
    REQUIRE(final_rss > 0);
    const double growth = static_cast<double>(final_rss) / static_cast<double>(baseline_rss) - 1.0;
    std::cout << "[AT-12] iterations=" << iterations << " baseline_rss=" << baseline_rss
              << " final_rss=" << final_rss << " growth=" << growth * 100.0
              << "% telemetry=" << telemetry_path.string() << '\n';
    INFO("AT-12 telemetry: " << telemetry_path.string());
    REQUIRE(growth <= 0.10);
}
