#if __has_include("AfarBuildInfo.h")
#include "AfarBuildInfo.h"
#endif

#include <catch2/catch_test_macros.hpp>

#include "DutSimulator.h"
#include "Manifest.h"
#include "MeasurementOrchestrator.h"
#include "ParquetExport.h"
#include "RawS21Store.h"
#include "RawS21TableExport.h"
#include "RunReportPdf.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"

#include <filesystem>
#include <fstream>

#ifndef AFAR_SOFTWARE_VERSION
#define AFAR_SOFTWARE_VERSION "0.1.0"
#endif
#ifndef AFAR_CXX_COMPILER_ID
#define AFAR_CXX_COMPILER_ID "unknown"
#endif
#ifndef AFAR_GIT_COMMIT
#define AFAR_GIT_COMMIT ""
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
    REQUIRE(std::filesystem::is_regular_file(series.rawS21CsvPath()));
    REQUIRE(series.rawS21CsvPath().filename() == afar::SeriesDirectory::kRawS21Csv);
    REQUIRE(series.rawS21CsvPath().filename() != afar::SeriesDirectory::kRawS21);
    {
        std::ifstream csv(series.rawS21CsvPath(), std::ios::binary);
        REQUIRE(csv);
        std::string header;
        REQUIRE(std::getline(csv, header));
        if (!header.empty() && header.back() == '\r') {
            header.pop_back();
        }
        REQUIRE(header
                == "run_id,timestamp_utc,channel,att_code,phase_code,freq_hz,"
                   "s21_re,s21_im,temp_c,attempt,overload,valid");
        std::string first_row;
        REQUIRE(std::getline(csv, first_row));
        REQUIRE(first_row.find(run_id) != std::string::npos);
    }
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
        REQUIRE(body.find(std::string("compiler: ") + AFAR_CXX_COMPILER_ID) != std::string::npos);
        REQUIRE(body.find("max_drift_phase_deg: 1") != std::string::npos);
        REQUIRE(body.find("max_phase_residual_deg: 2.8125") != std::string::npos);
        const std::string thru =
            "THRU: \xD0\xBD\xD0\xB5 \xD0\xB8\xD0\xB7\xD0\xBC\xD0\xB5\xD1\x80\xD0\xB5\xD0\xBD / "
            "\xD0\xB7\xD0\xBD\xD0\xB0\xD1\x87\xD0\xB5\xD0\xBD\xD0\xB8\xD1\x8F "
            "\xD0\xBC\xD0\xB0\xD1\x81\xD1\x82\xD0\xB5\xD1\x80\xD0\xB0";
        REQUIRE(body.find(thru) != std::string::npos);
        const std::string hash_missing =
            "\xD1\x85\xD0\xB5\xD1\x88 \xD0\xBD\xD0\xB5 \xD0\xB2\xD1\x88\xD0\xB8\xD1\x82";
        if (std::string(AFAR_GIT_COMMIT).empty()) {
            REQUIRE(body.find(hash_missing) != std::string::npos);
        } else {
            REQUIRE(body.find(AFAR_GIT_COMMIT) != std::string::npos);
        }
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
