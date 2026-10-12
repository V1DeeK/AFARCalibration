#include <catch2/catch_test_macros.hpp>

#include "PsSimulator.h"
#include "PsPacketPage.h"
#include "PsServiceCheckPage.h"
#include "PsServiceCheckRunner.h"
#include "PsTab.h"

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

}  // namespace

TEST_CASE("Вкладка PS: имитатор, сервисный режим, коды и модуль", "[ps][ui]")
{
    app();
    PsTab tab;
    tab.show();

    auto* writeCodes = child<QPushButton>(tab, "psWriteCodes");
    auto* toService = child<QPushButton>(tab, "psToService");
    CHECK_FALSE(writeCodes->isEnabled());

    child<QCheckBox>(tab, "psUseSimulator")->setChecked(true);
    child<QPushButton>(tab, "psConnect")->click();
    REQUIRE(waitFor([&] { return tab.link()->isReady(); }));
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

    // Модуль 0x01: частота синтезатора и включение — статус приходит с Lock.
    auto* table = child<QTableWidget>(tab, "psModuleTable");
    QDoubleSpinBox* freq = nullptr;
    QCheckBox* enSynth = nullptr;
    for (int row = 0; row < table->rowCount(); ++row) {
        const QString name = table->item(row, 1)->text();
        if (name.contains(QStringLiteral("(FREQ_ADF4355)"))) {
            freq = static_cast<QDoubleSpinBox*>(table->cellWidget(row, 2));
        } else if (name.contains(QStringLiteral("(EN_ADF4355)"))) {
            enSynth = static_cast<QCheckBox*>(table->cellWidget(row, 2));
        }
    }
    REQUIRE(freq);
    REQUIRE(enSynth);
    freq->setValue(5500.0);
    enSynth->setChecked(true);
    child<QPushButton>(tab, "psModuleWrite")->click();
    REQUIRE(waitFor([&] {
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 1)->text().contains(QStringLiteral("(FREQ_ADF4355)"))) {
                return static_cast<QLabel*>(table->cellWidget(row, 3))->text().startsWith(QStringLiteral("5500"));
            }
        }
        return false;
    }));

    // Серия занимает PS: связь вкладки закрывается и после серии восстанавливается.
    tab.setSeriesActive(true);
    CHECK_FALSE(writeCodes->isEnabled());
    CHECK_FALSE(child<QPushButton>(tab, "psToIdle")->isEnabled());
    CHECK_FALSE(child<QPushButton>(tab, "psConnect")->isEnabled());
    CHECK(tab.link()->state() == PsLink::State::Disconnected);
    tab.setSeriesActive(false);
    REQUIRE(waitFor([&] { return writeCodes->isEnabled(); }));

    child<QPushButton>(tab, "psConnect")->click();
    CHECK(tab.link()->state() == PsLink::State::Disconnected);
    CHECK_FALSE(writeCodes->isEnabled());
}

TEST_CASE("Вкладка PS: приёмка сервиса на имитаторе проходит целиком", "[ps][ui][check]")
{
    app();
    PsTab tab;
    tab.show();
    child<QCheckBox>(tab, "psUseSimulator")->setChecked(true);
    child<QPushButton>(tab, "psConnect")->click();
    REQUIRE(waitFor([&] { return tab.link()->isReady(); }));

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
    child<QCheckBox>(tab, "psUseSimulator")->setChecked(true);
    child<QPushButton>(tab, "psConnect")->click();
    REQUIRE(waitFor([&] { return tab.link()->isReady(); }));

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
