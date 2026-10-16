#include <catch2/catch_test_macros.hpp>

#include "PsLink.h"
#include "TestPsServer.h"

#include <QCoreApplication>
#include <QElapsedTimer>

#include <functional>
#include <vector>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

QCoreApplication& app()
{
    static int argc = 1;
    static char name[] = "test_ps_link";
    static char* argv[] = {name, nullptr};
    // Как в qt_test_application.h: не разрушаем до выхода процесса.
    static QCoreApplication& a = *new QCoreApplication(argc, argv);
    return a;
}

bool waitFor(const std::function<bool()>& cond, int timeoutMs = 3000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

struct Responses {
    std::vector<std::pair<ps::Envelope, ps::Status>> items;

    explicit Responses(PsLink& link)
    {
        QObject::connect(&link, &PsLink::responseReceived, &link,
                         [this](const ps::Envelope& e, ps::Status s) { items.emplace_back(e, s); });
    }
    const std::pair<ps::Envelope, ps::Status>* find(int count) const
    {
        for (const auto& r : items) {
            if (r.first.msg_count == count) {
                return &r;
            }
        }
        return nullptr;
    }
};

const std::pair<ps::Envelope, ps::Status>* await(Responses& r, int count)
{
    waitFor([&] { return r.find(count) != nullptr; });
    return r.find(count);
}

void connectToSim(PsLink& link, TestPsServer& sim)
{
    REQUIRE(sim.listen());
    link.connectToPs(QStringLiteral("127.0.0.1"), sim.port(), 0);
    REQUIRE(waitFor([&] { return link.isReady(); }));
}

}  // namespace

TEST_CASE("PS-связь: hello, телеметрия и переход в SERVICE", "[ps][link]")
{
    app();
    TestPsServer sim;
    sim.setTelemetryPeriodMs(50);
    PsLink link;
    Responses r(link);
    int telemetry = 0;
    QObject::connect(&link, &PsLink::telemetryReceived, &link,
                     [&](const ps::Telemetry&) { ++telemetry; });

    connectToSim(link, sim);
    CHECK(link.peerVersion() == QStringLiteral("1.0"));
    REQUIRE(waitFor([&] { return telemetry >= 2; }));
    CHECK(link.telemetryRejected() == 0);

    const int before = link.sendCommand(ps::msg::SetPhaseAtten, json::object());
    const auto* rejected = await(r, before);
    REQUIRE(rejected);
    CHECK(rejected->second == ps::Status::NotSupported);

    const int setMode = link.sendCommand(ps::msg::SetMode, json{{"mode", "SERVICE"}});
    const auto* modeReply = await(r, setMode);
    REQUIRE(modeReply);
    CHECK(modeReply->second == ps::Status::Ok);
    CHECK(sim.mode() == ps::Mode::Service);

    const int state = link.sendCommand(ps::msg::GetState);
    const auto* stateReply = await(r, state);
    REQUIRE(stateReply);
    ps::StateInfo info;
    REQUIRE(ps::StateInfo::fromJson(stateReply->first.payload, info));
    CHECK(info.mode == ps::Mode::Service);
}

TEST_CASE("PS-связь: коды решётки записываются и читаются обратно", "[ps][link]")
{
    app();
    TestPsServer sim;
    PsLink link;
    Responses r(link);
    connectToSim(link, sim);
    REQUIRE(await(r, link.sendCommand(ps::msg::SetMode, json{{"mode", "SERVICE"}})));

    std::vector<ps::ChannelCodes> codes;
    for (int n = 1; n <= ps::kChannels; ++n) {
        codes.push_back({n, n * 4 - 1, 64 - n * 2});
    }
    json payload;
    REQUIRE(ps::channelsToJson(codes, payload));
    const auto* set = await(r, link.sendCommand(ps::msg::SetPhaseAtten, payload));
    REQUIRE(set);
    REQUIRE(set->second == ps::Status::Ok);
    std::vector<ps::ChannelCodes> readback;
    REQUIRE(ps::channelsFromJson(set->first.payload, readback));
    CHECK(readback == codes);

    const auto* get = await(r, link.sendCommand(ps::msg::GetPhaseAtten));
    REQUIRE(get);
    REQUIRE(ps::channelsFromJson(get->first.payload, readback));
    CHECK(readback == codes);

    // Неполный массив PS отклоняет и не меняет решётку.
    payload["ch"].erase(payload["ch"].begin());
    const auto* bad = await(r, link.sendCommand(ps::msg::SetPhaseAtten, payload));
    REQUIRE(bad);
    CHECK(bad->second == ps::Status::InvalidArgument);
    CHECK(sim.codes()[0].ph == codes[0].ph);
}

TEST_CASE("PS-связь v8: состояние трансивера и rs485_send", "[ps][link]")
{
    app();
    TestPsServer sim;
    PsLink link;
    Responses r(link);
    connectToSim(link, sim);
    REQUIRE(await(r, link.sendCommand(ps::msg::SetMode, json{{"mode", "SERVICE"}})));

    const auto* trx = await(r, link.sendCommand(ps::msg::GetTrxStatus));
    REQUIRE(trx);
    CHECK(trx->first.payload.value("spi_ok", 0) == 1);

    // rs485_send из v2: PS подтверждает передачу байт в линию.
    const auto* sent = await(r, link.sendCommand(ps::msg::Rs485Send, json{{"data", "01A3FF"}}));
    REQUIRE(sent);
    CHECK(sent->second == ps::Status::Ok);
    CHECK(sent->first.payload.value("data", "") == "01A3FF");
}

TEST_CASE("PS-связь: молчащий PS — тайм-аут команды и разрыв по keep_alive", "[ps][link]")
{
    app();
    TestPsServer sim;
    PsLink link;
    PsLink::Timeouts t;
    t.commandMs = 300;
    t.keepAliveIdleMs = 400;
    t.reconnectMs = 60000;
    link.setTimeouts(t);
    QString lastTimeout;
    QObject::connect(&link, &PsLink::commandTimedOut, &link,
                     [&](const QString& type, int) { lastTimeout = type; });
    connectToSim(link, sim);

    sim.setSilent(true);
    REQUIRE(link.sendCommand(ps::msg::GetState) >= 0);
    REQUIRE(waitFor([&] { return !lastTimeout.isEmpty(); }));
    CHECK(lastTimeout == QStringLiteral("get_state"));

    REQUIRE(waitFor([&] { return link.state() == PsLink::State::Disconnected; }, 6000));
    CHECK_FALSE(link.lastError().isEmpty());
}

TEST_CASE("PS-связь: нет PS по адресу — ошибка без зависания", "[ps][link]")
{
    app();
    PsLink link;
    PsLink::Timeouts t;
    t.connectMs = 500;
    t.reconnectMs = 60000;
    link.setTimeouts(t);
    TestPsServer probe;
    REQUIRE(probe.listen());
    const quint16 freePort = probe.port();
    probe.close();

    link.connectToPs(QStringLiteral("127.0.0.1"), freePort, 0);
    REQUIRE(waitFor([&] { return !link.lastError().isEmpty(); }));
    CHECK(link.state() != PsLink::State::Ready);
    CHECK(link.sendCommand(ps::msg::GetState) == -1);
    link.disconnectFromPs();
    CHECK(link.state() == PsLink::State::Disconnected);
}

TEST_CASE("PS-связь: на адрес, который ни разу не ответил, повторных попыток нет", "[ps][link]")
{
    app();
    PsLink link;
    PsLink::Timeouts t;
    t.connectMs = 300;
    t.reconnectMs = 150;  // если бы повтор был, он случился бы за это время
    link.setTimeouts(t);
    TestPsServer probe;
    REQUIRE(probe.listen());
    const quint16 freePort = probe.port();
    probe.close();

    int attempts = 0;
    QObject::connect(&link, &PsLink::stateChanged, &link, [&](PsLink::State s) {
        if (s == PsLink::State::Connecting) {
            ++attempts;
        }
    });
    link.connectToPs(QStringLiteral("127.0.0.1"), freePort, 0);
    REQUIRE(waitFor([&] { return link.state() == PsLink::State::Disconnected && !link.lastError().isEmpty(); }));
    // Ждём заведомо дольше reconnectMs: связь остаётся разорванной, попытка одна.
    waitFor([&] { return false; }, 700);
    CHECK(link.state() == PsLink::State::Disconnected);
    CHECK_FALSE(link.isTrying());
    CHECK(attempts == 1);
}

TEST_CASE("PS-связь: установленная связь после обрыва восстанавливается сама", "[ps][link]")
{
    app();
    TestPsServer sim;
    PsLink link;
    PsLink::Timeouts t;
    t.reconnectMs = 150;
    t.connectMs = 400;
    link.setTimeouts(t);
    connectToSim(link, sim);
    const quint16 port = sim.port();

    sim.close();
    REQUIRE(waitFor([&] { return link.state() != PsLink::State::Ready; }));
    // Пока PS нет, связь «пытается»: интерфейс показывает «Отмена», а не «Подключиться».
    REQUIRE(waitFor([&] { return link.isTrying(); }));

    REQUIRE(sim.listen(port));
    CHECK(waitFor([&] { return link.isReady(); }, 5000));
}

TEST_CASE("PS-связь: подписчик на разрыв может сразу начать новое подключение", "[ps][link]")
{
    app();
    TestPsServer dead;
    REQUIRE(dead.listen());
    const quint16 deadPort = dead.port();
    dead.close();
    TestPsServer sim;
    REQUIRE(sim.listen());

    PsLink link;
    PsLink::Timeouts t;
    t.connectMs = 400;
    t.reconnectMs = 60000;
    t.commandMs = 300;
    t.keepAliveIdleMs = 200;
    link.setTimeouts(t);
    // Как поиск платы по MAC: адрес находится мгновенно и подключение идёт из обработчика разрыва.
    bool redirected = false;
    QObject::connect(&link, &PsLink::stateChanged, &link, [&](PsLink::State s) {
        if (s == PsLink::State::Disconnected && !redirected) {
            redirected = true;
            link.connectToPs(QStringLiteral("127.0.0.1"), sim.port(), 0);
        }
    });
    link.connectToPs(QStringLiteral("127.0.0.1"), deadPort, 0);
    CHECK(waitFor([&] { return link.isReady(); }, 4000));
    // Таймеры новой попытки живы: молчащий PS обнаруживается, а не висит вечно.
    sim.setSilent(true);
    REQUIRE(link.sendCommand(ps::msg::GetState) >= 0);
    CHECK(waitFor([&] { return link.state() == PsLink::State::Disconnected; }, 8000));
}
