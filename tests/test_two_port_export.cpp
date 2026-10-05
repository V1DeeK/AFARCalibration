#include "TwoPortExport.h"

#include <catch2/catch_test_macros.hpp>

#include <QGuiApplication>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

TEST_CASE("two-port Touchstone and PDF use one complete sweep", "[two_port_export]")
{
    int argc = 1;
    char applicationName[] = "test_two_port_export";
    char* argv[] = {applicationName, nullptr};
    QGuiApplication application(argc, argv);

    afar::report::TwoPortMeasurement measurement;
    measurement.requested.f_start_hz = 1'000'000'000ULL;
    measurement.requested.f_stop_hz = 2'000'000'000ULL;
    measurement.requested.points = 3;
    measurement.requested.ifbw_hz = 1000;
    measurement.requested.power_dbm = -20.0;
    measurement.requested.averages = 1;
    measurement.reference_ohm = 50.0;
    measurement.demo_mode = true;
    measurement.vna_idn = "PLANAR,C2220,SN1,1.0";
    measurement.measured_utc = "2026-09-28T12:00:00.000Z";
    measurement.device_name = "DUT-A";
    measurement.device_serial = "DUT-42";
    measurement.operator_name = "Operator";
    measurement.comment = "Control sweep";
    measurement.operator_accepted = true;
    measurement.sweep.frequency_hz = {1'000'000'000ULL, 1'500'000'000ULL,
                                      2'000'000'000ULL};
    measurement.sweep.s11 = {{0.1, 0.0}, {0.2, 0.0}, {0.3, 0.0}};
    measurement.sweep.s21 = {{0.8, 0.1}, {0.7, 0.2}, {0.6, 0.3}};
    measurement.sweep.s12 = {{0.01, 0.0}, {0.02, 0.0}, {0.03, 0.0}};
    measurement.sweep.s22 = {{0.15, 0.0}, {0.25, 0.0}, {0.35, 0.0}};
    measurement.marker_frequency_hz = {1'500'000'000ULL};

    std::string diagnostics;
    REQUIRE(afar::report::validateTwoPortMeasurement(measurement, diagnostics));

    const char* qaDirectory = std::getenv("AFAR_TWO_PORT_QA_DIR");
    const auto root = qaDirectory != nullptr
        ? std::filesystem::path(qaDirectory)
        : std::filesystem::temp_directory_path() / "afar-two-port-export";
    std::filesystem::create_directories(root);
    const auto s2p = root / "measurement.s2p";
    const auto pdf = root / "measurement.pdf";
    REQUIRE(afar::report::writeTouchstoneS2p(s2p, measurement, diagnostics));
    REQUIRE(afar::report::writeTwoPortReportPdf(pdf, measurement, diagnostics));

    {
        std::ifstream touchstone(s2p, std::ios::binary);
        std::ostringstream touchstoneText;
        touchstoneText << touchstone.rdbuf();
        REQUIRE(touchstoneText.str().find("# Hz S RI R 50") != std::string::npos);
        REQUIRE(touchstoneText.str().find("PLANAR,C2220,SN1,1.0") != std::string::npos);
        REQUIRE(touchstoneText.str().find("S2VNA_DEMO_NON_METROLOGICAL")
                != std::string::npos);
        REQUIRE(touchstoneText.str().find("device_serial=DUT-42") != std::string::npos);
        REQUIRE(touchstoneText.str().find("operator_assessment=ACCEPTED")
                != std::string::npos);
    }
    {
        std::ifstream report(pdf, std::ios::binary);
        std::ostringstream reportBytes;
        reportBytes << report.rdbuf();
        REQUIRE(reportBytes.str().starts_with("%PDF-"));
        REQUIRE(reportBytes.str().find("/Count 6") != std::string::npos);
        REQUIRE(reportBytes.str().size() > 20'000);
    }

    const auto blockedParent = root / "not-a-directory";
    {
        std::ofstream blocker(blockedParent, std::ios::binary | std::ios::trunc);
        blocker << "file";
    }
    REQUIRE_FALSE(afar::report::writeTwoPortReportPdf(
        blockedParent / "report.pdf", measurement, diagnostics));
    REQUIRE_FALSE(diagnostics.empty());

    auto invalid = measurement;
    invalid.sweep.frequency_hz[1] = invalid.sweep.frequency_hz[0];
    REQUIRE_FALSE(afar::report::validateTwoPortMeasurement(invalid, diagnostics));
    REQUIRE(diagnostics.find("strictly increasing") != std::string::npos);

    if (qaDirectory == nullptr) {
        std::filesystem::remove_all(root);
    }
}
