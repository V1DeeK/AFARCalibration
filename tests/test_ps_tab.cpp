#include <catch2/catch_test_macros.hpp>

#include "PsSimulator.h"
#include "PsTab.h"

#include <QApplication>
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
