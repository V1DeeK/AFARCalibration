#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "MeasurementOrchestrator.h"
#include "PsDutController.h"
#include "TestPsServer.h"
#include "VnaSimulator.h"
#include "probe_fixtures.h"
#include "qt_test_application.h"

#include <QMetaObject>
#include <QThread>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>

using Catch::Matchers::ContainsSubstring;
namespace ps = afar::ps;

namespace {

void app()
{
    // Отчёт PDF серии требует QGuiApplication — один экземпляр на процесс.
    (void)afar::test::guiApplication();
}

/// Имитатор PS в своём потоке: драйвер блокирует поток теста.
class SimThread {
public:
    SimThread(const SimThread&) = delete;
    SimThread& operator=(const SimThread&) = delete;

    SimThread()
    {
        anchor_.moveToThread(&thread_);
        thread_.start();
        QMetaObject::invokeMethod(
            &anchor_,
            [this] {
                sim_ = new TestPsServer;
                sim_->setTelemetryPeriodMs(50);
                port_ = sim_->listen() ? sim_->port() : 0;
            },
            Qt::BlockingQueuedConnection);
    }
    ~SimThread()
    {
        QMetaObject::invokeMethod(&anchor_, [this] { delete sim_; }, Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
    }
    quint16 port() const { return port_; }
    template <typename F>
    auto call(F f)
    {
        decltype(f(*sim_)) out{};
        QMetaObject::invokeMethod(&anchor_, [&] { out = f(*sim_); }, Qt::BlockingQueuedConnection);
        return out;
    }

private:
    struct Anchor : QObject {};
    QThread thread_;
    Anchor anchor_;
    TestPsServer* sim_ = nullptr;
    quint16 port_ = 0;
};

PsDutController::Options optionsFor(const SimThread& sim)
{
    PsDutController::Options o;
    o.host = "127.0.0.1";
    o.port = sim.port();
    o.telemetry_port = 0;
    o.timeout_ms = 1500;
    return o;
}

}  // namespace

TEST_CASE("PS-контроллер: SERVICE, коды активного канала и 63/0 на остальных", "[ps][dut]")
{
    app();
    SimThread sim;
    REQUIRE(sim.port() != 0);
    PsDutController dut(optionsFor(sim));
    dut.connect();
    CHECK(dut.connected());
    CHECK(sim.call([](TestPsServer& s) { return s.mode() == ps::Mode::Service; }));

    DutState st{};
    st.channel = 7;
    st.att_code = 18;
    st.phase_code = 42;
    dut.apply(st);
    const auto codes = sim.call([](TestPsServer& s) { return s.codes(); });
    for (const auto& c : codes) {
        if (c.n == 7) {
            CHECK(c.att == 18);
            CHECK(c.ph == 42);
        } else {
            CHECK(c.att == 63);
            CHECK(c.ph == 0);
        }
    }
    const DutState rb = dut.readback();
    CHECK(rb.channel == 7);
    CHECK(rb.att_code == 18);
    CHECK(rb.phase_code == 42);

    const double t = dut.temperature_c();
    CHECK(t == 37.4); // максимум из четырёх РЧ-модулей, безопасный для контроля нагрева

    dut.set_safe_state();
    const auto safe = sim.call([](TestPsServer& s) { return s.codes(); });
    for (const auto& c : safe) {
        CHECK(c.att == 63);
        CHECK(c.ph == 0);
    }
}

TEST_CASE("PS-контроллер: отказы дают исключение, а не тихую ошибку", "[ps][dut]")
{
    app();
    SimThread sim;
    PsDutController dut(optionsFor(sim));

    SECTION("коды вне диапазона не уходят в PS")
    {
        dut.connect();
        DutState bad{};
        bad.channel = 17;
        CHECK_THROWS_WITH(dut.apply(bad), ContainsSubstring("вне диапазона"));
    }
    SECTION("PS в FAULT не переходит в SERVICE")
    {
        sim.call([](TestPsServer& s) {
            s.setFault(true);
            return 0;
        });
        CHECK_THROWS_WITH(dut.connect(), ContainsSubstring("IDLE/READY"));
    }
    SECTION("молчащий PS — тайм-аут")
    {
        dut.connect();
        sim.call([](TestPsServer& s) {
            s.setSilent(true);
            return 0;
        });
        DutState st{};
        st.channel = 1;
        CHECK_THROWS_WITH(dut.apply(st), ContainsSubstring("нет ответа"));
        dut.set_safe_state();  // не бросает
    }
    SECTION("нет PS по адресу")
    {
        auto o = optionsFor(sim);
        o.port = 1;
        o.timeout_ms = 500;
        PsDutController nobody(o);
        CHECK_THROWS(nobody.connect());
        CHECK_FALSE(nobody.connected());
    }
}

TEST_CASE("PS-контроллер в серии AT-03: оркестратор пишет коды через PS", "[ps][dut][AT-03]")
{
    app();
    SimThread sim;
    const auto root = std::filesystem::temp_directory_path() / "afar_ps_probe_run";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto fixtures = root / "fixtures";
    afar::test::writeProbeFixtures(fixtures, "RX16-20261012-001", 11);
    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    std::string diag;
    REQUIRE(afar::test::loadProbeConfig(fixtures, cfg, att, diag));

    // Оркестратор держит raw-s21.h5 открытым до разрушения; на Windows
    // каталог удаляется только после этого.
    {
        VnaSimulator vna;
        PsDutController dut(optionsFor(sim));
        afar::MeasurementOrchestrator orch(&vna, &dut);
        orch.setConfig(cfg, att);
        orch.setSleepEnabled(false);
        REQUIRE(orch.prepare(root / "data", fixtures / "run-config.json",
                             fixtures / "attenuator-codes.csv", diag));
        CHECK(dut.connected());
        REQUIRE(orch.start(diag));
        orch.runUntilDone();

        REQUIRE(orch.state() == afar::RunState::Complete);
        CHECK(orch.store().completedCount() == 8);
        // После серии решётка в безопасном состоянии: 63/0 на всех каналах.
        const auto codes = sim.call([](TestPsServer& s) { return s.codes(); });
        for (const auto& c : codes) {
            CHECK(c.att == 63);
            CHECK(c.ph == 0);
        }
    }
    std::filesystem::remove_all(root, ec);
}
