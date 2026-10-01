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
#include "qt_test_application.h"

#include <algorithm>
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
    (void)afar::test::guiApplication();
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
    const auto channel_calibration = series.root() / "channel-01-calibration.parquet";
    const auto channel_report = series.root() / "channel-01-report.pdf";
    REQUIRE(std::filesystem::is_regular_file(channel_calibration));
    REQUIRE(std::filesystem::is_regular_file(channel_report));
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
    std::vector<afar::report::InverseLutEntry> channel_inverse;
    REQUIRE(afar::report::readInverseLut(channel_calibration, channel_inverse, diag));
    REQUIRE(channel_inverse.size() == inverse2.size());
    REQUIRE(std::all_of(channel_inverse.begin(), channel_inverse.end(), [](const auto& row) {
        return row.channel == 1;
    }));
    REQUIRE(afar::report::isValidPdfSmoke(channel_report, diag));

    // Закрыть store перед reopen/verify (файл на диске стабилен).
    orch.store().close();

    afar::RawS21Store reopened;
    REQUIRE(afar::RawS21Store::open(series.rawS21Path(), reopened, diag));
    REQUIRE(reopened.completedCount() == completed);
    reopened.close();

    REQUIRE(afar::report::verifyManifest(series, diag));

    // PDF теперь рисуется Qt с Unicode-шрифтом: проверяем структуру, содержание — через
    // рассчитанные выше direct/inverse и визуальную QA отчёта.
    {
        std::ifstream pdf(series.reportPath(), std::ios::binary);
        REQUIRE(pdf);
        std::string body((std::istreambuf_iterator<char>(pdf)),
                         std::istreambuf_iterator<char>());
        REQUIRE(body.size() > 10'000);
        REQUIRE(body.find("/Count 2") != std::string::npos);
    }

    // Пересчёт SHA одной строки манифеста совпадает.
    std::vector<afar::report::ManifestEntry> entries;
    REQUIRE(afar::report::readManifestFile(series.manifestPath(), entries, diag));
    REQUIRE(entries.size() >= 5);
    bool found_direct = false;
    bool found_channel_calibration = false;
    bool found_channel_report = false;
    for (const auto& e : entries) {
        const auto hex = afar::report::sha256FileHex(series.root() / e.filename, diag);
        REQUIRE(hex == e.hex);
        if (e.filename == afar::SeriesDirectory::kDirectLut) {
            found_direct = true;
        }
        found_channel_calibration = found_channel_calibration
            || e.filename == "channel-01-calibration.parquet";
        found_channel_report = found_channel_report
            || e.filename == "channel-01-report.pdf";
    }
    REQUIRE(found_direct);
    REQUIRE(found_channel_calibration);
    REQUIRE(found_channel_report);
}
