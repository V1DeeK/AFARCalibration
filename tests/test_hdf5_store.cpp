#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "RawS21Store.h"
#include "RunEventLog.h"
#include "SeriesDirectory.h"
#include "probe_fixtures.h"

#include <complex>
#include <filesystem>
#include <fstream>
#include <vector>

TEST_CASE("SeriesDirectory create copies config and forbids escape", "[hdf5_store][DATA-005]")
{
    const auto root = std::filesystem::temp_directory_path() / "afar_series_dir_test";
    std::filesystem::remove_all(root);
    const auto fixtures = root / "fixtures";
    afar::test::writeProbeFixtures(fixtures, "RX16-20260922-001");

    afar::SeriesDirectory series;
    std::string diag;
    REQUIRE(afar::SeriesDirectory::create(root / "data", "RX16-20260922-001",
                                          fixtures / "run-config.json",
                                          fixtures / "attenuator-codes.csv", series, diag));
    REQUIRE(std::filesystem::exists(series.runConfigPath()));
    REQUIRE(std::filesystem::exists(series.attenuatorCodesPath()));
    REQUIRE(series.rawS21Path().filename() == "raw-s21.h5");
    REQUIRE(series.runEventsPath().filename() == "run-events.jsonl");

    afar::SeriesDirectory bad;
    REQUIRE_FALSE(afar::SeriesDirectory::create(root / "data", "../escape",
                                                fixtures / "run-config.json",
                                                fixtures / "attenuator-codes.csv", bad, diag));
}

TEST_CASE("RawS21Store create/write/reopen/completed", "[hdf5_store][DATA-006][TEST-003]")
{
    const auto path =
        std::filesystem::temp_directory_path() / "afar_raw_s21_store_test.h5";
    std::filesystem::remove(path);

    std::vector<std::uint8_t> channels{1};
    std::vector<std::uint16_t> atts{0, 1};
    std::vector<std::uint8_t> phases{0, 1};
    std::vector<std::uint64_t> freqs{1000, 2000, 3000};

    afar::RawS21Store store;
    std::string diag;
    REQUIRE(afar::RawS21Store::create(path, channels, atts, phases, freqs, store, diag));

    afar::RawS21StateRecord rec;
    rec.s21 = {{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}};
    rec.valid = {1, 1, 0};
    rec.temperature_c = 25.5f;
    rec.overload = false;
    rec.attempt = 1;
    rec.completed = true;
    REQUIRE(store.writeState(1, 0, 0, rec, diag));
    REQUIRE(store.isCompleted(1, 0, 0));
    REQUIRE(store.completedCount() == 1);

    // completed=true нельзя переписать
    rec.attempt = 2;
    REQUIRE_FALSE(store.writeState(1, 0, 0, rec, diag));

    store.close();

    afar::RawS21Store reopened;
    REQUIRE(afar::RawS21Store::open(path, reopened, diag));
    afar::RawS21StateRecord got;
    REQUIRE(reopened.readState(1, 0, 0, got, diag));
    REQUIRE(got.completed);
    REQUIRE(got.attempt == 1);
    REQUIRE(got.s21.size() == 3);
    REQUIRE(got.s21[0].real() == Catch::Approx(1.0));
    REQUIRE(got.s21[2].imag() == Catch::Approx(6.0));
    REQUIRE(reopened.completedCount() == 1);

    // clearCompleted снимает AT-05 блок и позволяет переписать слот
    REQUIRE(reopened.clearCompleted(1, 0, 0, diag));
    REQUIRE_FALSE(reopened.isCompleted(1, 0, 0));
    rec.attempt = 2;
    rec.completed = true;
    REQUIRE(reopened.writeState(1, 0, 0, rec, diag));
    REQUIRE(reopened.isCompleted(1, 0, 0));
    REQUIRE(reopened.completedCount() == 1);

    // incomplete можно переписать
    afar::RawS21StateRecord rec2 = rec;
    rec2.completed = false;
    rec2.attempt = 1;
    REQUIRE(reopened.writeState(1, 1, 1, rec2, diag));
    rec2.attempt = 2;
    rec2.completed = true;
    REQUIRE(reopened.writeState(1, 1, 1, rec2, diag));
    REQUIRE(reopened.completedCount() == 2);
}

TEST_CASE("RunEventLog append JSONL without S21 arrays", "[hdf5_store][DATA-007]")
{
    const auto path =
        std::filesystem::temp_directory_path() / "afar_run_events_test.jsonl";
    std::filesystem::remove(path);

    afar::RunEventLog log;
    std::string diag;
    REQUIRE(afar::RunEventLog::openAppend(path, log, diag));

    afar::RunEvent ev;
    ev.time_utc = "2026-09-22T12:00:00.000Z";
    ev.level = afar::EventLevel::Info;
    ev.component = "test";
    ev.event_code = "UNIT";
    ev.run_id = "RX16-20260922-001";
    ev.channel = 1;
    ev.att_code = 0;
    ev.phase_code = 2;
    ev.attempt = 1;
    ev.message = "hello";
    REQUIRE(log.append(ev, diag));
    log.close();

    std::ifstream in(path);
    std::string line;
    REQUIRE(std::getline(in, line));
    REQUIRE(line.find("\"message\":\"hello\"") != std::string::npos);
    REQUIRE(line.find("s21") == std::string::npos);
}
