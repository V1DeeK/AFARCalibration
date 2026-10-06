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
#include <cmath>
#include <cstdlib>
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
    const auto channel_calibration_csv = series.root() / "channel-01-calibration.csv";
    const auto channel_report = series.root() / "channel-01-report.pdf";
    REQUIRE(std::filesystem::is_regular_file(channel_calibration));
    REQUIRE(std::filesystem::is_regular_file(channel_calibration_csv));
    REQUIRE_FALSE(std::filesystem::exists(channel_calibration_csv.string() + ".tmp"));
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
    REQUIRE(std::all_of(direct2.begin(), direct2.end(), [](const auto& row) {
        return std::isfinite(row.repeatability_db)
            && std::isfinite(row.repeatability_deg);
    }));
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
    std::string csv_run_id;
    std::vector<afar::report::InverseLutEntry> channel_inverse_csv;
    REQUIRE(afar::report::readInverseLutCsv(channel_calibration_csv, csv_run_id,
                                            channel_inverse_csv, diag));
    REQUIRE(csv_run_id == run_id);
    REQUIRE(channel_inverse_csv.size() == channel_inverse.size());
    REQUIRE(afar::report::countValidInverse(channel_inverse_csv)
            == afar::report::countValidInverse(channel_inverse));
    REQUIRE(std::equal(channel_inverse.begin(), channel_inverse.end(),
                       channel_inverse_csv.begin(), [](const auto& left, const auto& right) {
                           return left.channel == right.channel
                               && left.freq_hz == right.freq_hz
                               && left.target_atten_db == right.target_atten_db
                               && left.target_phase_deg == right.target_phase_deg
                               && left.selected_att_code == right.selected_att_code
                               && left.selected_phase_code == right.selected_phase_code
                               && left.valid == right.valid;
                       }));
    {
        std::ifstream csv(channel_calibration_csv, std::ios::binary);
        std::string header;
        REQUIRE(std::getline(csv, header));
        REQUIRE(header
                == "run_id,channel,freq_hz,target_atten_db,target_phase_deg,selected_att_code,"
                   "selected_phase_code,measured_atten_db,measured_phase_deg,atten_residual_db,"
                   "phase_residual_deg,valid");
    }
    REQUIRE(afar::report::isValidPdfSmoke(channel_report, diag));
    {
        std::ifstream pdf(channel_report, std::ios::binary);
        const std::string body((std::istreambuf_iterator<char>(pdf)),
                               std::istreambuf_iterator<char>());
        REQUIRE(body.find("/Count 5") != std::string::npos);
    }

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
    bool found_channel_calibration_csv = false;
    bool found_channel_report = false;
    for (const auto& e : entries) {
        const auto hex = afar::report::sha256FileHex(series.root() / e.filename, diag);
        REQUIRE(hex == e.hex);
        if (e.filename == afar::SeriesDirectory::kDirectLut) {
            found_direct = true;
        }
        found_channel_calibration = found_channel_calibration
            || e.filename == "channel-01-calibration.parquet";
        found_channel_calibration_csv = found_channel_calibration_csv
            || e.filename == "channel-01-calibration.csv";
        found_channel_report = found_channel_report
            || e.filename == "channel-01-report.pdf";
    }
    REQUIRE(found_direct);
    REQUIRE(found_channel_calibration);
    REQUIRE(found_channel_calibration_csv);
    REQUIRE(found_channel_report);
}

TEST_CASE("channel report has 64x64 matrices and incomplete coverage cannot pass",
          "[channel_report]")
{
    (void)afar::test::guiApplication();
    afar::report::RunReportInfo info;
    info.detailed_channel_report = true;
    info.run_id = "RX16-CHANNEL-REPORT-QA";
    info.series_path = "C:/data/RX16-CHANNEL-REPORT-QA";
    info.vna_idn = "PLANAR,C2220,DEMO,26.3";
    info.demo_mode = true;
    info.frequency_points = 201;
    info.f_start_hz = 4'900'000'000ULL;
    info.f_stop_hz = 6'000'000'000ULL;
    info.ifbw_hz = 1000;
    info.power_dbm = -30.0;
    info.averages = 4;

    afar::report::ChannelReportInfo channel;
    channel.channel = 1;
    channel.expected_states = 4096;
    channel.completed_states = 4095;
    channel.valid_states = 4095;
    channel.valid_direct_count = 4095 * info.frequency_points;
    channel.invalid_direct_count = info.frequency_points;
    channel.valid_inverse_count = 4095;
    channel.center_frequency_hz = 5'450'000'000ULL;
    channel.max_atten_error_db = 0.18;
    channel.max_phase_error_deg = 2.4;
    channel.max_repeatability_db = 0.07;
    channel.max_repeatability_deg = 0.8;
    channel.max_drift_phase_deg = 0.6;
    channel.overload_states = 0;
    channel.retry_count = 3;
    channel.calibration_filename = "channel-01-calibration.parquet";
    channel.calibration_sha256 = std::string(64, 'a');
    channel.calibration_csv_filename = "channel-01-calibration.csv";
    channel.calibration_csv_sha256 = std::string(64, 'c');
    channel.raw_filename = "raw-s21.h5";
    channel.raw_sha256 = std::string(64, 'b');
    for (std::uint16_t code = 0; code < 64; ++code) {
        channel.att_codes.push_back(code);
        channel.phase_codes.push_back(static_cast<std::uint8_t>(code));
    }
    for (std::uint16_t att = 0; att < 64; ++att) {
        for (std::uint8_t phase = 0; phase < 64; ++phase) {
            afar::report::ChannelStateReport state;
            state.att_code = att;
            state.phase_code = phase;
            state.completed = !(att == 63 && phase == 63);
            state.valid = state.completed;
            state.center_value_available = state.completed;
            state.center_atten_db = 0.5 * att + 0.02 * std::sin(phase / 5.0);
            state.center_phase_error_deg = 2.0 * std::sin((att + phase) / 9.0);
            state.worst_frequency_hz = 5'450'000'000ULL;
            state.worst_atten_error_db = 0.02 * std::sin(phase / 5.0);
            state.worst_phase_error_deg = state.center_phase_error_deg;
            channel.states.push_back(state);
        }
    }
    for (int index = 0; index < 20; ++index) {
        channel.worst_states.push_back({static_cast<std::uint16_t>(index),
                                        static_cast<std::uint8_t>(63 - index),
                                        5'450'000'000ULL,
                                        0.01 * index,
                                        0.1 * index,
                                        index != 0});
    }
    REQUIRE_FALSE(afar::report::channelReportPasses(channel));
    auto complete = channel;
    complete.completed_states = complete.expected_states;
    complete.valid_states = complete.expected_states;
    REQUIRE(afar::report::channelReportPasses(complete));
    complete.error_states = 1;
    REQUIRE_FALSE(afar::report::channelReportPasses(complete));

    info.channels = {channel};
    const char* qaDirectory = std::getenv("AFAR_CHANNEL_REPORT_QA_DIR");
    const auto root = qaDirectory != nullptr
        ? std::filesystem::path(qaDirectory)
        : std::filesystem::temp_directory_path() / "afar-channel-report";
    std::filesystem::create_directories(root);
    const auto pdf = root / "channel-01-report.pdf";
    std::string diagnostics;
    REQUIRE(afar::report::writeRunReportPdf(pdf, info, diagnostics));
    REQUIRE(afar::report::isValidPdfSmoke(pdf, diagnostics));
    {
        std::ifstream report(pdf, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(report)),
                                std::istreambuf_iterator<char>());
        REQUIRE(bytes.find("/Count 5") != std::string::npos);
        REQUIRE(bytes.size() > 20'000);
    }
    if (qaDirectory == nullptr) {
        std::filesystem::remove_all(root);
    }
}
