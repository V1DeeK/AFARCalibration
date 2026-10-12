#include <catch2/catch_test_macros.hpp>

#include "PsPacketPage.h"
#include "PsServiceCheckPage.h"
#include "PsServiceCheckRunner.h"
#include "PsTab.h"
#include "TestPsServer.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QUdpSocket>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

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

/// Подключить вкладку к тестовому серверу так же, как оператор к PS: адрес и порты.
void connectTab(PsTab& tab, TestPsServer& server)
{
    REQUIRE(server.listen());
    QUdpSocket probe;
    REQUIRE(probe.bind(QHostAddress::LocalHost, 0));
    const quint16 tlmPort = probe.localPort();
    probe.close();
    child<QLineEdit>(tab, "psHost")->setText(QStringLiteral("127.0.0.1"));
    child<QSpinBox>(tab, "psPort")->setValue(server.port());
    child<QSpinBox>(tab, "psTlmPort")->setValue(tlmPort);
    child<QPushButton>(tab, "psConnect")->click();
    REQUIRE(waitFor([&] { return tab.link()->isReady(); }));
}

int countRows(QTableWidget* t, const QString& channel, const QString& dir, const QString& type)
{
    int n = 0;
    for (int row = 0; row < t->rowCount(); ++row) {
        if (t->item(row, 1)->text() == channel && t->item(row, 2)->text().contains(dir) &&
            t->item(row, 3)->text() == type) {
            ++n;
        }
    }
    return n;
}

}  // namespace

TEST_CASE("Вкладка PS: подключение по адресу, сервисный режим, коды и обмен TCP и UDP", "[ps][ui]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(100);
    PsTab tab;
    tab.show();

    auto* writeCodes = child<QPushButton>(tab, "psWriteCodes");
    auto* toService = child<QPushButton>(tab, "psToService");
    CHECK_FALSE(writeCodes->isEnabled());
    connectTab(tab, server);
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

    // В таблице обмена видно и отправленное, и пришедшее — по TCP и по UDP.
    auto* ex = tab.exchangeTable();
    CHECK(countRows(ex, QStringLiteral("TCP"), QStringLiteral("ушло"), QStringLiteral("set_phase_atten")) == 1);
    CHECK(countRows(ex, QStringLiteral("TCP"), QStringLiteral("пришло"), QStringLiteral("set_phase_atten")) == 1);
    CHECK(countRows(ex, QStringLiteral("TCP"), QStringLiteral("ушло"), QStringLiteral("hello")) == 1);
    REQUIRE(waitFor([&] {
        return countRows(ex, QStringLiteral("UDP"), QStringLiteral("пришло"), QStringLiteral("telemetry")) >= 2;
    }));
    for (int row = 0; row < ex->rowCount(); ++row) {
        if (ex->item(row, 3)->text() == QStringLiteral("set_phase_atten") &&
            ex->item(row, 2)->text().contains(QStringLiteral("пришло"))) {
            CHECK(ex->item(row, 5)->text() == QStringLiteral("OK"));
            CHECK(ex->item(row, 6)->text().contains(QStringLiteral("\"ph\":42")));
        }
    }

    // Серия, которая пишет коды через PS, занимает связь; после серии — снова на связи.
    tab.setSeriesActive(true);
    CHECK_FALSE(writeCodes->isEnabled());
    CHECK_FALSE(child<QPushButton>(tab, "psConnect")->isEnabled());
    CHECK(tab.link()->state() == PsLink::State::Disconnected);
    tab.setSeriesActive(false);
    REQUIRE(waitFor([&] { return writeCodes->isEnabled(); }));

    child<QPushButton>(tab, "psConnect")->click();
    CHECK(tab.link()->state() == PsLink::State::Disconnected);
    CHECK_FALSE(writeCodes->isEnabled());
}

TEST_CASE("Вкладка PS: без адреса не подключается, имитатора нет", "[ps][ui]")
{
    app();
    PsTab tab;
    CHECK(tab.findChild<QCheckBox*>(QStringLiteral("psUseSimulator")) == nullptr);
    child<QLineEdit>(tab, "psHost")->clear();
    child<QPushButton>(tab, "psConnect")->click();
    CHECK(tab.link()->state() == PsLink::State::Disconnected);
}

TEST_CASE("Вкладка PS: приёмка проверяет TCP и UDP и проходит целиком", "[ps][ui][check]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(100);
    PsTab tab;
    tab.show();
    connectTab(tab, server);

    auto* start = child<QPushButton>(tab, "psCheckStart");
    REQUIRE(waitFor([&] { return start->isEnabled(); }));
    start->click();
    CHECK(tab.checkPage()->isRunning());
    // Пока идёт приёмка, ручные команды заблокированы.
    CHECK_FALSE(child<QPushButton>(tab, "psToService")->isEnabled());
    CHECK_FALSE(child<QPushButton>(tab, "psPacketSend")->isEnabled());
    REQUIRE(waitFor([&] { return !tab.checkPage()->isRunning(); }, 20000));

    const auto* runner = tab.checkPage()->runner();
    int udpSteps = 0;
    for (std::size_t i = 0; i < runner->outcomes().size(); ++i) {
        INFO("шаг " << i + 1 << ": " << runner->steps()[i].title << " — "
                    << runner->outcomes()[i].result.detail);
        CHECK(runner->outcomes()[i].result.verdict == afar::ps::CheckVerdict::Pass);
        if (runner->steps()[i].kind != afar::ps::CheckKind::Command) {
            ++udpSteps;
        }
    }
    CHECK(udpSteps == 4);
    CHECK(child<QLabel>(tab, "psCheckSummary")->text().contains(QStringLiteral("все шаги выполнены")));
    CHECK(server.mode() == afar::ps::Mode::Idle);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString report = dir.filePath(QStringLiteral("check.md"));
    REQUIRE(tab.checkPage()->saveReport(report));
    QFile f(report);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    CHECK(md.contains(QStringLiteral("пройдено %1 из %1").arg(runner->steps().size())));
    CHECK(md.contains(QStringLiteral("UDP: телеметрия в режиме SERVICE")));
    CHECK(child<QPushButton>(tab, "psPacketSend")->isEnabled());
}

TEST_CASE("Вкладка PS: приёмка не проходит, если UDP-телеметрия не приходит", "[ps][ui][check]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(0);  // PS не шлёт телеметрию
    PsTab tab;
    connectTab(tab, server);
    child<QPushButton>(tab, "psCheckStart")->click();
    REQUIRE(waitFor([&] { return !tab.checkPage()->isRunning(); }, 30000));
    const auto* runner = tab.checkPage()->runner();
    for (std::size_t i = 0; i < runner->steps().size(); ++i) {
        if (runner->steps()[i].kind == afar::ps::CheckKind::TelemetryMode) {
            CHECK(runner->outcomes()[i].result.verdict == afar::ps::CheckVerdict::NoAnswer);
        }
    }
}

TEST_CASE("Вкладка PS: конструктор пакетов и наборы кодов в файле", "[ps][ui]")
{
    app();
    TestPsServer server;
    PsTab tab;
    tab.show();
    connectTab(tab, server);

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
