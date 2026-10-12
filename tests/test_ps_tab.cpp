#include <catch2/catch_test_macros.hpp>

#include "PsSimulator.h"
#include "PsPacketPage.h"
#include "PsServiceCheckPage.h"
#include "PsServiceCheckRunner.h"
#include "PsTab.h"
#include "PsTrafficView.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QUdpSocket>

#include <functional>

namespace {

QApplication& app()
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    static int argc = 1;
    static char name[] = "test_ps_tab";
    static char* argv[] = {name, nullptr};
    // Не разрушаем до выхода процесса — см. qt_test_application.h.
    static QApplication& a = *new QApplication(argc, argv);
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

template <typename T>
T* child(QWidget& w, const char* name)
{
    T* c = w.findChild<T*>(QString::fromLatin1(name));
    REQUIRE(c != nullptr);
    return c;
}

/// Свободный UDP-порт для телеметрии: занятый 49601 не должен ронять тест.
int freeUdpPort()
{
    QUdpSocket probe;
    REQUIRE(probe.bind(QHostAddress::LocalHost, 0));
    return probe.localPort();
}

/// Вкладка подключается к ответчику PS так же, как к PS изделия: по адресу.
void connectTab(PsTab& tab, PsSimulator& sim)
{
    REQUIRE((sim.isListening() || sim.listen()));
    child<QLineEdit>(tab, "psHost")->setText(QStringLiteral("127.0.0.1"));
    child<QSpinBox>(tab, "psPort")->setValue(sim.port());
    child<QSpinBox>(tab, "psTlmPort")->setValue(freeUdpPort());
    child<QPushButton>(tab, "psConnect")->click();
    REQUIRE(waitFor([&] { return tab.link()->isReady(); }));
}

}  // namespace

TEST_CASE("Вкладка PS: подключение по адресу, сервисный режим, коды и обмен", "[ps][ui]")
{
    app();
    PsTab tab;
    tab.show();

    auto* writeCodes = child<QPushButton>(tab, "psWriteCodes");
    auto* toService = child<QPushButton>(tab, "psToService");
    CHECK_FALSE(writeCodes->isEnabled());
    // Имитатора PS в программе нет: только адрес PS изделия.
    CHECK(tab.findChild<QCheckBox*>(QStringLiteral("psUseSimulator")) == nullptr);
    CHECK(tab.findChild<QTableWidget*>(QStringLiteral("psModuleTable")) == nullptr);

    PsSimulator sim;
    connectTab(tab, sim);
    REQUIRE(waitFor([&] { return toService->isEnabled(); }));
    CHECK_FALSE(writeCodes->isEnabled());

    toService->click();
    REQUIRE(waitFor([&] { return writeCodes->isEnabled(); }));
    CHECK(child<QLabel>(tab, "psMode")->text().contains(QStringLiteral("сервисный")));

    auto* codes = child<QTableWidget>(tab, "psCodes");
    static_cast<QSpinBox*>(codes->cellWidget(4, 1))->setValue(42);
    static_cast<QSpinBox*>(codes->cellWidget(4, 2))->setValue(18);
    writeCodes->click();
    REQUIRE(waitFor([&] { return codes->item(4, 3)->text() == QStringLiteral("42"); }));
    CHECK(codes->item(4, 4)->text() == QStringLiteral("18"));

    // Обмен: команда и ответ в одной паре, телеметрия идёт по UDP.
    auto* traffic = tab.trafficView();
    REQUIRE(waitFor([&] {
        for (const auto& f : traffic->frames()) {
            if (f.outgoing && f.msgType == QStringLiteral("set_phase_atten") && f.status == QStringLiteral("OK")) {
                return f.pair >= 0 && traffic->frames()[static_cast<std::size_t>(f.pair)].msgClass ==
                                          QStringLiteral("response");
            }
        }
        return false;
    }));
    REQUIRE(waitFor([&] { return traffic->udpReceived() > 0; }));
    CHECK(traffic->tcpSent() >= 3);
    CHECK(traffic->tcpTimeouts() == 0);
    QTemporaryDir logDir;
    REQUIRE(logDir.isValid());
    const QString logPath = logDir.filePath(QStringLiteral("exchange.log"));
    REQUIRE(traffic->saveLog(logPath));
    QFile logFile(logPath);
    REQUIRE(logFile.open(QIODevice::ReadOnly));
    const QString logText = QString::fromUtf8(logFile.readAll());
    CHECK(logText.contains(QStringLiteral("\tTCP\t->\t{")));
    CHECK(logText.contains(QStringLiteral("\tUDP\t<-\t{")));

    // Серия (VNA подключён, идёт измерение) связь с PS не рвёт: блокируются
    // только ручные команды.
    tab.setSeriesActive(true);
    CHECK_FALSE(writeCodes->isEnabled());
    CHECK_FALSE(child<QPushButton>(tab, "psToIdle")->isEnabled());
    CHECK(child<QPushButton>(tab, "psConnect")->isEnabled());
    CHECK(tab.link()->isReady());
    const auto udpBefore = tab.link()->telemetryPackets();
    REQUIRE(waitFor([&] { return tab.link()->telemetryPackets() > udpBefore; }));
    tab.setSeriesActive(false);
    CHECK(writeCodes->isEnabled());

    child<QPushButton>(tab, "psConnect")->click();
    CHECK(tab.link()->state() == PsLink::State::Disconnected);
    CHECK_FALSE(writeCodes->isEnabled());
}

TEST_CASE("Вкладка PS: приёмка сервиса TCP и UDP проходит целиком", "[ps][ui][check]")
{
    app();
    PsTab tab;
    tab.show();
    PsSimulator sim;
    connectTab(tab, sim);

    auto* start = child<QPushButton>(tab, "psCheckStart");
    REQUIRE(waitFor([&] { return start->isEnabled(); }));
    start->click();
    CHECK(tab.checkPage()->isRunning());
    // Пока идёт приёмка, ручные команды заблокированы.
    CHECK_FALSE(child<QPushButton>(tab, "psToService")->isEnabled());
    CHECK_FALSE(child<QPushButton>(tab, "psPacketSend")->isEnabled());
    REQUIRE(waitFor([&] { return !tab.checkPage()->isRunning(); }, 15000));

    const auto* runner = tab.checkPage()->runner();
    for (std::size_t i = 0; i < runner->outcomes().size(); ++i) {
        INFO("шаг " << i + 1 << ": " << runner->steps()[i].title << " — "
                    << runner->outcomes()[i].result.detail);
        CHECK(runner->outcomes()[i].result.verdict == afar::ps::CheckVerdict::Pass);
    }
    CHECK(child<QLabel>(tab, "psCheckSummary")->text().contains(QStringLiteral("все шаги выполнены")));

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString report = dir.filePath(QStringLiteral("check.md"));
    REQUIRE(tab.checkPage()->saveReport(report));
    QFile f(report);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    CHECK(md.contains(QStringLiteral("пройдено %1 из %1").arg(runner->steps().size())));
    CHECK(child<QPushButton>(tab, "psPacketSend")->isEnabled());
}

TEST_CASE("Вкладка PS: конструктор пакетов и наборы кодов в файле", "[ps][ui]")
{
    app();
    PsTab tab;
    tab.show();
    PsSimulator sim;
    connectTab(tab, sim);

    auto* type = child<QComboBox>(tab, "psPacketType");
    auto* payload = child<QPlainTextEdit>(tab, "psPacketPayload");
    auto* result = child<QLabel>(tab, "psPacketResult");
    type->setCurrentText(QStringLiteral("get_beam"));
    REQUIRE(tab.packetPage()->sendPacket());
    // Вне SERVICE PS отклоняет сервисную команду — это и видно в конструкторе.
    REQUIRE(waitFor([&] { return result->text().contains(QStringLiteral("NOT_SUPPORTED")); }));

    type->setCurrentText(QStringLiteral("set_mode"));
    payload->setPlainText(QStringLiteral("{\"mode\": \"SERVICE\"}"));
    REQUIRE(tab.packetPage()->sendPacket());
    REQUIRE(waitFor([&] { return child<QPushButton>(tab, "psWriteCodes")->isEnabled(); }));

    payload->setPlainText(QStringLiteral("[1, 2]"));
    CHECK_FALSE(tab.packetPage()->sendPacket());
    CHECK(result->text().startsWith(QStringLiteral("Не отправлено")));

    auto* codes = child<QTableWidget>(tab, "psCodes");
    static_cast<QSpinBox*>(codes->cellWidget(2, 1))->setValue(21);
    static_cast<QSpinBox*>(codes->cellWidget(2, 2))->setValue(33);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("codes.json"));
    REQUIRE(tab.saveCodes(path));
    static_cast<QSpinBox*>(codes->cellWidget(2, 1))->setValue(0);
    static_cast<QSpinBox*>(codes->cellWidget(2, 2))->setValue(0);
    REQUIRE(tab.loadCodes(path));
    CHECK(static_cast<QSpinBox*>(codes->cellWidget(2, 1))->value() == 21);
    CHECK(static_cast<QSpinBox*>(codes->cellWidget(2, 2))->value() == 33);

    const QString broken = dir.filePath(QStringLiteral("broken.json"));
    QFile f(broken);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(R"({"ch":[{"n":1,"ph":0,"att":64}]})");
    f.close();
    QString error;
    CHECK_FALSE(tab.loadCodes(broken, &error));
    CHECK(error.startsWith(QStringLiteral("Набор кодов отклонён")));
    CHECK(static_cast<QSpinBox*>(codes->cellWidget(2, 1))->value() == 21);
}
