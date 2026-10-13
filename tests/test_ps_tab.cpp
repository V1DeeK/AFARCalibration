#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "ConnectionTab.h"
#include "FullScanPanel.h"
#include "MacResolver.h"
#include "ParametersTab.h"
#include "PsJournalDialog.h"
#include "PsModulesPage.h"
#include "PsPacketPage.h"
#include "PsServiceCheckPage.h"
#include "PsServiceCheckRunner.h"
#include "PsSession.h"
#include "StressTestPanel.h"
#include "TestPsServer.h"
#include "TestRunTab.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QUdpSocket>

#include <functional>
#include <memory>

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
    // Настройки теста — отдельно от настроек программы оператора.
    QCoreApplication::setOrganizationName(QStringLiteral("AFAR-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("test_ps_tab"));
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

/// Вкладки, которые делят одну связь с PS, — как в главном окне.
struct Ui {
    Ui()
    {
        QSettings().clear();
        session = std::make_unique<PsSession>();
        connection = std::make_unique<ConnectionTab>(session.get());
        parameters = std::make_unique<ParametersTab>(session.get());
        testRun = std::make_unique<TestRunTab>(session.get());
        check = std::make_unique<PsServiceCheckPage>(session->link());
        QObject::connect(check.get(), &PsServiceCheckPage::runningChanged, session.get(),
                         &PsSession::setCheckRunning);
        QObject::connect(session.get(), &PsSession::controlsChanged, check.get(), [this] {
            check->setAllowed(session->link()->isReady() && !session->seriesActive());
        });
    }
    ~Ui()
    {
        check.reset();
        testRun.reset();
        parameters.reset();
        connection.reset();
        session.reset();
    }
    void show()
    {
        connection->show();
        parameters->show();
    }

    std::unique_ptr<PsSession> session;
    std::unique_ptr<ConnectionTab> connection;
    std::unique_ptr<ParametersTab> parameters;
    std::unique_ptr<TestRunTab> testRun;
    std::unique_ptr<PsServiceCheckPage> check;
};

/// Подключиться к тестовому серверу так же, как оператор к PS: адрес и порты.
void connectUi(Ui& ui, TestPsServer& server)
{
    REQUIRE(server.listen());
    QUdpSocket probe;
    REQUIRE(probe.bind(QHostAddress::LocalHost, 0));
    const quint16 tlmPort = probe.localPort();
    probe.close();
    QWidget& c = *ui.connection;
    child<QLineEdit>(c, "psHost")->setText(QStringLiteral("127.0.0.1"));
    child<QLineEdit>(c, "psMac")->clear();
    child<QSpinBox>(c, "psPort")->setValue(server.port());
    child<QSpinBox>(c, "psTlmPort")->setValue(tlmPort);
    child<QPushButton>(c, "psConnect")->click();
    REQUIRE(waitFor([&] { return ui.session->link()->isReady(); }));
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

TEST_CASE("Установка параметров: подключение, сервисный режим, коды и обмен TCP и UDP", "[ps][ui]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(100);
    Ui ui;
    ui.show();
    QWidget& p = *ui.parameters;

    auto* writeCodes = child<QPushButton>(p, "psWriteCodes");
    auto* toService = child<QPushButton>(p, "psToService");
    CHECK_FALSE(writeCodes->isEnabled());
    connectUi(ui, server);
    REQUIRE(waitFor([&] { return toService->isEnabled(); }));
    CHECK_FALSE(writeCodes->isEnabled());
    CHECK(child<QLabel>(*ui.connection, "psLinkState")->text().startsWith(QStringLiteral("на связи")));

    toService->click();
    REQUIRE(waitFor([&] { return writeCodes->isEnabled(); }));
    CHECK(child<QLabel>(p, "psMode")->text().contains(QStringLiteral("сервисный")));

    auto* codes = child<QTableWidget>(p, "psCodes");
    static_cast<QSpinBox*>(codes->cellWidget(4, 1))->setValue(42);
    static_cast<QSpinBox*>(codes->cellWidget(4, 2))->setValue(18);
    writeCodes->click();
    REQUIRE(waitFor([&] { return codes->item(4, 3)->text() == QStringLiteral("42"); }));
    CHECK(codes->item(4, 4)->text() == QStringLiteral("18"));

    // В таблице обмена видно и отправленное, и пришедшее — по TCP и по UDP.
    auto* ex = ui.parameters->exchangeTable();
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
    // Та же связь видна и в журнале «Тестового прогона».
    auto* journal = child<QTableWidget>(*ui.testRun, "testRunJournal");
    CHECK(countRows(journal, QStringLiteral("TCP"), QStringLiteral("ушло"), QStringLiteral("set_phase_atten")) == 1);

    // Техсостояние из телеметрии.
    REQUIRE(waitFor([&] {
        return child<QLabel>(*ui.connection, "psTechValues")->text().contains(QStringLiteral("Работа"));
    }));

    // Серия, которая пишет коды через PS, занимает связь; после серии — снова на связи.
    ui.session->setSeriesActive(true);
    CHECK_FALSE(writeCodes->isEnabled());
    CHECK_FALSE(child<QPushButton>(*ui.connection, "psConnect")->isEnabled());
    CHECK(ui.session->link()->state() == PsLink::State::Disconnected);
    ui.session->setSeriesActive(false);
    REQUIRE(waitFor([&] { return writeCodes->isEnabled(); }));

    child<QPushButton>(*ui.connection, "psConnect")->click();
    CHECK(ui.session->link()->state() == PsLink::State::Disconnected);
    CHECK_FALSE(writeCodes->isEnabled());
    CHECK_FALSE(ui.session->autoConnect());
}

TEST_CASE("Подключение: без адреса и MAC не подключается, неверный MAC отклоняется", "[ps][ui]")
{
    app();
    Ui ui;
    QWidget& c = *ui.connection;
    CHECK(c.findChild<QCheckBox*>(QStringLiteral("psUseSimulator")) == nullptr);
    child<QLineEdit>(c, "psHost")->clear();
    child<QLineEdit>(c, "psMac")->clear();
    child<QPushButton>(c, "psConnect")->click();
    CHECK(ui.session->link()->state() == PsLink::State::Disconnected);
    CHECK_FALSE(ui.session->searching());

    child<QLineEdit>(c, "psMac")->setText(QStringLiteral("12:34"));
    child<QPushButton>(c, "psConnect")->click();
    CHECK_FALSE(ui.session->searching());
    CHECK(child<QLabel>(c, "psMessage")->text().contains(QStringLiteral("MAC")));
}

TEST_CASE("Поиск платы по MAC: таблица ARP Windows, macOS и Linux", "[ps][mac]")
{
    CHECK(MacResolver::normalizeMac(QStringLiteral("42:7e:d7:4e:0f:77")) == QStringLiteral("427ED74E0F77"));
    CHECK(MacResolver::normalizeMac(QStringLiteral("0:a:35:2:f3:a8")) == QStringLiteral("000A3502F3A8"));
    CHECK(MacResolver::formatMac(QStringLiteral("427ed74e0f77")) == QStringLiteral("42:7E:D7:4E:0F:77"));
    CHECK_FALSE(MacResolver::isValidMac(QStringLiteral("42:7e:d7")));

    const QString windows = QStringLiteral(
        "Interface: 192.168.1.5 --- 0x7\n"
        "  Internet Address      Physical Address      Type\n"
        "  192.168.1.20          42-7e-d7-4e-0f-77     dynamic\n"
        "  192.168.1.255         ff-ff-ff-ff-ff-ff     static\n");
    const QString macos = QStringLiteral(
        "? (192.168.1.1) at 0:11:22:33:44:55 on en0 ifscope [ethernet]\n"
        "? (192.168.1.31) at 42:7e:d7:4e:f:77 on en0 ifscope [ethernet]\n"
        "? (192.168.1.40) at (incomplete) on en0 ifscope [ethernet]\n");
    const QString linux = QStringLiteral("? (10.0.0.9) at 42:7e:d7:4e:0f:77 [ether] on eth0\n");

    CHECK(MacResolver::parseArpOutput(windows).value(QStringLiteral("427ED74E0F77")) == QStringLiteral("192.168.1.20"));
    CHECK_FALSE(MacResolver::parseArpOutput(windows).contains(QStringLiteral("FFFFFFFFFFFF")));
    const auto mac = MacResolver::parseArpOutput(macos);
    CHECK(mac.value(QStringLiteral("427ED74E0F77")) == QStringLiteral("192.168.1.31"));
    CHECK(mac.value(QStringLiteral("001122334455")) == QStringLiteral("192.168.1.1"));
    CHECK(mac.size() == 2);
    CHECK(MacResolver::parseArpOutput(linux).value(QStringLiteral("427ED74E0F77")) == QStringLiteral("10.0.0.9"));
}

TEST_CASE("Приёмка PS проверяет TCP и UDP и проходит целиком", "[ps][ui][check]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(100);
    Ui ui;
    ui.show();
    ui.check->show();
    connectUi(ui, server);

    auto* start = child<QPushButton>(*ui.check, "psCheckStart");
    REQUIRE(waitFor([&] { return start->isEnabled(); }));
    start->click();
    CHECK(ui.check->isRunning());
    // Пока идёт приёмка, ручные команды заблокированы.
    CHECK_FALSE(child<QPushButton>(*ui.parameters, "psToService")->isEnabled());
    CHECK_FALSE(child<QPushButton>(*ui.connection, "psPacketSend")->isEnabled());
    REQUIRE(waitFor([&] { return !ui.check->isRunning(); }, 20000));

    const auto* runner = ui.check->runner();
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
    CHECK(child<QLabel>(*ui.check, "psCheckSummary")->text().contains(QStringLiteral("все шаги выполнены")));
    CHECK(server.mode() == afar::ps::Mode::Idle);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString report = dir.filePath(QStringLiteral("check.md"));
    REQUIRE(ui.check->saveReport(report));
    QFile f(report);
    REQUIRE(f.open(QIODevice::ReadOnly));
    const QString md = QString::fromUtf8(f.readAll());
    CHECK(md.contains(QStringLiteral("пройдено %1 из %1").arg(runner->steps().size())));
    CHECK(md.contains(QStringLiteral("UDP: телеметрия в режиме SERVICE")));
    CHECK(child<QPushButton>(*ui.connection, "psPacketSend")->isEnabled());
}

TEST_CASE("Приёмка PS не проходит, если UDP-телеметрия не приходит", "[ps][ui][check]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(0);  // PS не шлёт телеметрию
    Ui ui;
    connectUi(ui, server);
    child<QPushButton>(*ui.check, "psCheckStart")->click();
    REQUIRE(waitFor([&] { return !ui.check->isRunning(); }, 30000));
    const auto* runner = ui.check->runner();
    for (std::size_t i = 0; i < runner->steps().size(); ++i) {
        if (runner->steps()[i].kind == afar::ps::CheckKind::TelemetryMode) {
            CHECK(runner->outcomes()[i].result.verdict == afar::ps::CheckVerdict::NoAnswer);
        }
    }
}

TEST_CASE("Одна команда: история, пакеты «Отправлено/Получено»; коды в файле", "[ps][ui]")
{
    app();
    TestPsServer server;
    Ui ui;
    ui.show();
    connectUi(ui, server);
    QWidget& c = *ui.connection;

    auto* type = child<QComboBox>(c, "psPacketType");
    auto* payload = child<QPlainTextEdit>(c, "psPacketPayload");
    auto* result = child<QLabel>(c, "psPacketResult");
    auto* history = child<QTableWidget>(c, "psPacketHistory");
    type->setCurrentText(QStringLiteral("get_beam"));
    REQUIRE(ui.connection->packetPage()->sendPacket());
    // Вне SERVICE PS отклоняет сервисную команду — это и видно в истории.
    REQUIRE(waitFor([&] { return result->text().contains(QStringLiteral("NOT_SUPPORTED")); }));
    CHECK(history->rowCount() == 1);
    CHECK(history->item(0, 1)->text() == QStringLiteral("get_beam"));
    CHECK(child<QPlainTextEdit>(c, "psPacketResponse")->toPlainText().contains(QStringLiteral("NOT_SUPPORTED")));
    CHECK(child<QPlainTextEdit>(c, "psPacketRequest")->toPlainText().contains(QStringLiteral("get_beam")));

    type->setCurrentText(QStringLiteral("set_mode"));
    payload->setPlainText(QStringLiteral("{\"mode\": \"SERVICE\"}"));
    REQUIRE(ui.connection->packetPage()->sendPacket());
    REQUIRE(waitFor([&] { return child<QPushButton>(*ui.parameters, "psWriteCodes")->isEnabled(); }));
    CHECK(history->rowCount() == 2);

    payload->setPlainText(QStringLiteral("[1, 2]"));
    CHECK_FALSE(ui.connection->packetPage()->sendPacket());
    CHECK(result->text().startsWith(QStringLiteral("Не отправлено")));

    auto* codes = child<QTableWidget>(*ui.parameters, "psCodes");
    static_cast<QSpinBox*>(codes->cellWidget(2, 1))->setValue(21);
    static_cast<QSpinBox*>(codes->cellWidget(2, 2))->setValue(33);
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("codes.json"));
    REQUIRE(ui.parameters->saveCodes(path));
    static_cast<QSpinBox*>(codes->cellWidget(2, 1))->setValue(0);
    static_cast<QSpinBox*>(codes->cellWidget(2, 2))->setValue(0);
    REQUIRE(ui.parameters->loadCodes(path));
    CHECK(static_cast<QSpinBox*>(codes->cellWidget(2, 1))->value() == 21);
    CHECK(static_cast<QSpinBox*>(codes->cellWidget(2, 2))->value() == 33);

    const QString broken = dir.filePath(QStringLiteral("broken.json"));
    QFile f(broken);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write(R"({"ch":[{"n":1,"ph":0,"att":64}]})");
    f.close();
    QString error;
    CHECK_FALSE(ui.parameters->loadCodes(broken, &error));
    CHECK(error.startsWith(QStringLiteral("Набор кодов отклонён")));
    CHECK(static_cast<QSpinBox*>(codes->cellWidget(2, 1))->value() == 21);
}

TEST_CASE("Журнал: вкладка «Журнал» открывает модальное окно", "[ps][ui]")
{
    app();
    TestPsServer server;
    Ui ui;
    ui.show();
    connectUi(ui, server);
    auto* tabs = child<QTabWidget>(*ui.connection, "connectionCommandTabs");
    REQUIRE(tabs->count() == 2);
    CHECK(tabs->tabText(0) == QStringLiteral("Одна команда"));
    CHECK(tabs->tabText(1) == QStringLiteral("Журнал"));
    tabs->setCurrentIndex(1);
    PsJournalDialog* journal = ui.connection->journal();
    REQUIRE(waitFor([&] { return journal->isVisible(); }));
    CHECK(journal->isModal());
    CHECK(tabs->currentIndex() == 0);
    auto* table = journal->findChild<QTableWidget*>(QStringLiteral("psJournalTable"));
    REQUIRE(table != nullptr);
    CHECK(countRows(table, QStringLiteral("TCP"), QStringLiteral("ушло"), QStringLiteral("hello")) == 1);
    journal->reject();
}

TEST_CASE("Полное сканирование: перевод в режим, параметры, старт и результаты", "[ps][ui][sff]")
{
    app();
    TestPsServer server;
    Ui ui;
    ui.show();
    connectUi(ui, server);
    QWidget& c = *ui.connection;

    child<QDoubleSpinBox>(c, "sffFreqFrom")->setValue(5000.0);  // МГц
    child<QComboBox>(c, "sffFreqToUnit")->setCurrentIndex(1);   // ГГц
    child<QDoubleSpinBox>(c, "sffFreqTo")->setValue(5.8);
    child<QDoubleSpinBox>(c, "sffAzFrom")->setValue(-20.0);
    child<QDoubleSpinBox>(c, "sffAzTo")->setValue(20.0);
    auto* start = child<QPushButton>(c, "sffStart");
    REQUIRE(waitFor([&] { return start->isEnabled(); }));
    start->click();
    auto* results = child<QTableWidget>(c, "sffResults");
    REQUIRE(waitFor([&] { return results->rowCount() == 3; }));
    CHECK(server.mode() == afar::ps::Mode::FullScanFreq);
    CHECK(server.sffParams().freq_min == 5'000'000'000LL);
    CHECK(server.sffParams().freq_max == 5'800'000'000LL);
    CHECK(server.sffParams().az_min == Catch::Approx(-20.0));
    CHECK(results->item(0, 0)->text() == QStringLiteral("5000.000"));
    CHECK(results->item(0, 1)->text() == QStringLiteral("нет"));
    CHECK(results->item(1, 1)->text() == QStringLiteral("есть"));
    CHECK(results->item(2, 2)->text() == QStringLiteral("20.0°"));
    CHECK(results->item(2, 3)->text() == QStringLiteral("62.0"));
    CHECK(child<QPushButton>(c, "sffStop")->isEnabled());

    // Вне диапазона — не отправляется.
    child<QDoubleSpinBox>(c, "sffAzFrom")->setValue(25.0);
    afar::ps::SffParams p;
    QString error;
    CHECK_FALSE(ui.connection->fullScan()->readParams(p, &error));
    CHECK(error.contains(QStringLiteral("сектор")));
}

TEST_CASE("Тестовый прогон: стресс-тест TCP и проверка потока UDP", "[ps][ui][stress]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(100);
    Ui ui;
    ui.show();
    connectUi(ui, server);
    QWidget& t = *ui.testRun;
    child<QSpinBox>(t, "stressRate")->setValue(50);
    child<QSpinBox>(t, "stressTotal")->setValue(30);
    child<QPushButton>(t, "stressStart")->click();
    REQUIRE(ui.testRun->stress()->running());
    REQUIRE(waitFor([&] { return !ui.testRun->stress()->running(); }, 15000));
    const QString summary = ui.testRun->stress()->summary();
    INFO(summary.toStdString());
    CHECK(summary.contains(QStringLiteral("отправлено 30 из 30")));
    CHECK(summary.contains(QStringLiteral("OK 4")));  // get_state и get_sff_parameters вне режима — отказы
    CHECK(summary.contains(QStringLiteral("нет ответа 0")));

    ui.testRun->startUdpCheck(2);
    REQUIRE(waitFor([&] { return !ui.testRun->udpCheckRunning(); }, 5000));
    CHECK(child<QLabel>(t, "udpVerdict")->text().startsWith(QStringLiteral("Норма")));
    CHECK(child<QTableWidget>(t, "udpDevices")->rowCount() > 0);
}

TEST_CASE("Модули — гетеродины, смеситель, видеоприёмник по JSON", "[ps][ui][modules]")
{
    app();
    TestPsServer server;
    Ui ui;
    ui.show();
    connectUi(ui, server);
    QWidget& tab = *ui.parameters;

    auto* lo1Write = child<QPushButton>(tab, "psLo1Write");
    CHECK_FALSE(lo1Write->isEnabled());  // только в SERVICE
    child<QPushButton>(tab, "psToService")->click();
    REQUIRE(waitFor([&] { return lo1Write->isEnabled(); }));
    auto* page = ui.parameters->modulesPage();
    // Гетеродины — два разных устройства на линии RS-485.
    CHECK(page->tabs()->tabToolTip(0).contains(QStringLiteral("0x01")));
    CHECK(page->tabs()->tabToolTip(1).contains(QStringLiteral("0x02")));
    CHECK(page->tabs()->tabText(4) == QStringLiteral("Коды решётки"));

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

    auto* ex = ui.parameters->exchangeTable();
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
