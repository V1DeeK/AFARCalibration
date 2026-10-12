#include <catch2/catch_test_macros.hpp>

#include "PsModulesPage.h"
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

TEST_CASE("Вкладка PS: модули — гетеродины, смеситель, видеоприёмник по JSON", "[ps][ui][modules]")
{
    app();
    TestPsServer server;
    PsTab tab;
    tab.show();
    connectTab(tab, server);

    auto* lo1Write = child<QPushButton>(tab, "psLo1Write");
    CHECK_FALSE(lo1Write->isEnabled());  // только в SERVICE
    child<QPushButton>(tab, "psToService")->click();
    REQUIRE(waitFor([&] { return lo1Write->isEnabled(); }));
    auto* page = tab.modulesPage();

    // Ничего не отмечено — не отправляем.
    CHECK_FALSE(page->write(PsModulesPage::Lo1));

    // Правка значения сама отмечает «Передать»; уходят только отмеченные поля.
    child<QDoubleSpinBox>(tab, "psLo1_freq")->setValue(5500.0);
    child<QDoubleSpinBox>(tab, "psLo1_att1")->setValue(0.5);
    CHECK(child<QCheckBox>(tab, "psLo1_freq_send")->isChecked());
    CHECK_FALSE(child<QCheckBox>(tab, "psLo1_att2_send")->isChecked());
    lo1Write->click();
    auto* lo1State = child<QLabel>(tab, "psLo1State");
    REQUIRE(waitFor([&] { return lo1State->text().startsWith(QStringLiteral("Ответ: OK")); }));
    CHECK(server.moduleState(1)["freq"] == 5'500'000'000LL);
    CHECK(server.moduleState(1)["att1"] == 0.5);
    CHECK(server.moduleState(1)["att2"] == 31.5);   // не передавали — не изменилось
    CHECK(server.moduleState(2)["freq"] == 0);      // второй гетеродин не тронут

    auto* ex = tab.exchangeTable();
    bool sentPartial = false;
    for (int row = 0; row < ex->rowCount(); ++row) {
        if (ex->item(row, 3)->text() == QStringLiteral("set_lo") &&
            ex->item(row, 2)->text().contains(QStringLiteral("ушло"))) {
            const QString j = ex->item(row, 6)->text();
            sentPartial = j.contains(QStringLiteral("\"freq\":5500000000")) &&
                          !j.contains(QStringLiteral("att2"));
        }
    }
    CHECK(sentPartial);

    child<QPushButton>(tab, "psLo2Read")->click();
    auto* lo2State = child<QLabel>(tab, "psLo2State");
    REQUIRE(waitFor([&] { return lo2State->text().contains(QStringLiteral("°C")); }));
    CHECK(lo2State->text().contains(QStringLiteral("захвата частоты нет")));
    CHECK_FALSE(child<QCheckBox>(tab, "psLo2_freq_send")->isChecked());  // чтение не отмечает

    child<QCheckBox>(tab, "psMixer3_send")->setChecked(true);
    child<QDoubleSpinBox>(tab, "psMixer3_if_att")->setValue(4.5);
    child<QCheckBox>(tab, "psMixer3_en")->setChecked(true);
    child<QPushButton>(tab, "psMixerWrite")->click();
    REQUIRE(waitFor([&] { return server.mixerState()["ch"][2]["if_att"] == 4.5; }));
    CHECK(server.mixerState()["ch"][2]["en"] == 1);
    CHECK(server.mixerState()["ch"][0]["en"] == 0);
    child<QPushButton>(tab, "psMixerRead")->click();
    REQUIRE(waitFor([&] { return child<QLabel>(tab, "psMixerState")->text().contains(QStringLiteral("°C")); }));

    child<QCheckBox>(tab, "psVideo_en_vid")->setChecked(true);
    child<QComboBox>(tab, "psVideo_pwr_a")->setCurrentIndex(3);
    child<QPushButton>(tab, "psVideoWrite")->click();
    REQUIRE(waitFor([&] { return server.moduleState(4)["en_vid"] == 1; }));
    CHECK(server.moduleState(4)["pwr_a"] == 5);

    // Модуль не ответил по RS-485 — PS отвечает HW_NOT_READY, это видно на странице.
    server.setModulesSilent(true);
    child<QPushButton>(tab, "psVideoRead")->click();
    REQUIRE(waitFor([&] {
        return child<QLabel>(tab, "psVideoState")->text().contains(QStringLiteral("HW_NOT_READY"));
    }));
}
