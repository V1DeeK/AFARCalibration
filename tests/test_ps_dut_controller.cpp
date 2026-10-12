#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "PsDutController.h"
#include "PsSimulator.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <atomic>
#include <memory>
#include <stdexcept>

using Catch::Matchers::ContainsSubstring;
namespace ps = afar::ps;

namespace {

QCoreApplication& app()
{
    static int argc = 1;
    static char name[] = "test_ps_dut_controller";
    static char* argv[] = {name, nullptr};
    // Не разрушаем до выхода процесса — см. qt_test_application.h.
    static QCoreApplication& a = *new QCoreApplication(argc, argv);
    return a;
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
                sim_ = new PsSimulator;
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
    PsSimulator* sim_ = nullptr;
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
    CHECK(sim.call([](PsSimulator& s) { return s.mode() == ps::Mode::Service; }));

    DutState st{};
    st.channel = 7;
    st.att_code = 18;
    st.phase_code = 42;
    dut.apply(st);
    const auto codes = sim.call([](PsSimulator& s) { return s.codes(); });
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
    CHECK(t > 0.0);

    dut.set_safe_state();
    const auto safe = sim.call([](PsSimulator& s) { return s.codes(); });
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
        sim.call([](PsSimulator& s) {
            s.setFault(true);
            return 0;
        });
        CHECK_THROWS_WITH(dut.connect(), ContainsSubstring("IDLE/READY"));
    }
    SECTION("молчащий PS — тайм-аут")
    {
        dut.connect();
        sim.call([](PsSimulator& s) {
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
