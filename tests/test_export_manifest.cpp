#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "Manifest.h"
#include "MeasurementOrchestrator.h"
#include "ParquetExport.h"
#include "RawS21Store.h"
#include "RunReportPdf.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"

#include <filesystem>
#include <fstream>

#ifndef AFAR_SOFTWARE_VERSION
#define AFAR_SOFTWARE_VERSION "0.1.0"
#endif

TEST_CASE("AT-11 export: parquet reopen, sha256, valid count",
          "[export_manifest][AT-11][TEST-010][RPT-001][RPT-002][RPT-003]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_export_manifest";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    const std::string run_id = "RX16-20260922-011";
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

    const auto completed = orch.store().completedCount();
    REQUIRE(completed == 8);

    auto& series = orch.series();
    // Оркестратор пишет артефакты в Finalizing (AT-11).
    REQUIRE(std::filesystem::is_regular_file(series.directLutPath()));
    REQUIRE(std::filesystem::is_regular_file(series.inverseLutPath()));
    REQUIRE(std::filesystem::is_regular_file(series.reportPath()));
    REQUIRE(std::filesystem::is_regular_file(series.manifestPath()));
    REQUIRE(afar::report::isValidPdfSmoke(series.reportPath(), diag));

    // --- DATA-03 / AT-11: reopen + sizes + checksum + valid ---
    std::vector<afar::cal::DirectLutEntry> direct2;
    REQUIRE(afar::report::readDirectLut(series.directLutPath(), direct2, diag));
    REQUIRE_FALSE(direct2.empty());
    const auto valid_direct = afar::report::countValidDirect(direct2);
    REQUIRE(valid_direct > 0);

    std::vector<afar::report::InverseLutEntry> inverse2;
    REQUIRE(afar::report::readInverseLut(series.inverseLutPath(), inverse2, diag));
    REQUIRE_FALSE(inverse2.empty());
    REQUIRE(afar::report::countValidInverse(inverse2) > 0);

    // Закрыть store перед reopen/verify (файл на диске стабилен).
    orch.store().close();

    afar::RawS21Store reopened;
    REQUIRE(afar::RawS21Store::open(series.rawS21Path(), reopened, diag));
    REQUIRE(reopened.completedCount() == completed);
    reopened.close();

    REQUIRE(afar::report::verifyManifest(series, diag));

    // Согласованность числа valid с PDF (текст содержит то же число).
    {
        std::ifstream pdf(series.reportPath(), std::ios::binary);
        REQUIRE(pdf);
        std::string body((std::istreambuf_iterator<char>(pdf)),
                         std::istreambuf_iterator<char>());
        const auto needle = "valid_direct_count: " + std::to_string(valid_direct);
        REQUIRE(body.find(needle) != std::string::npos);
        REQUIRE(body.find(run_id) != std::string::npos);
        REQUIRE(body.find(AFAR_SOFTWARE_VERSION) != std::string::npos);
        REQUIRE(body.find("completed_states: " + std::to_string(completed))
                != std::string::npos);
    }

    // Пересчёт SHA одной строки манифеста совпадает.
    std::vector<afar::report::ManifestEntry> entries;
    REQUIRE(afar::report::readManifestFile(series.manifestPath(), entries, diag));
    REQUIRE(entries.size() >= 5);
    bool found_direct = false;
    for (const auto& e : entries) {
        const auto hex = afar::report::sha256FileHex(series.root() / e.filename, diag);
        REQUIRE(hex == e.hex);
        if (e.filename == afar::SeriesDirectory::kDirectLut) {
            found_direct = true;
        }
    }
    REQUIRE(found_direct);
}
