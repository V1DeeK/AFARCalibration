// Проверки главного окна целиком — тем, что видит и делает оператор: вкладки, ввод с клавиатуры,
// подключение к PS, темы, перекрытие и сжатие виджетов на разных размерах окна.
// Снимки вкладок для проверки внешнего вида: AFAR_TEST_SHOTS_DIR=<каталог> test_main_window "[shots]".

#include <catch2/catch_test_macros.hpp>

#include "MacResolver.h"
#include "MainWindow.h"
#include "MeasureTab.h"
#include "PsSession.h"
#include "SingleChannelPanel.h"
#include "S2VnaGraphHost.h"
#include "TestPsServer.h"
#include "Theme.h"
#include "UiStyle.h"
#include "spectrum_engine.hpp"
#include "spectrum_opengl_widget.hpp"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QOpenGLWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUdpSocket>
#include <QtTest/QtTest>

#include <algorithm>
#include <functional>
#include <memory>

namespace {

QApplication& app()
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    static int argc = 1;
    static char name[] = "test_main_window";
    static char* argv[] = {name, nullptr};
    static QApplication& a = *new QApplication(argc, argv);
    // Настройки теста — во временном каталоге, чтобы не читать и не менять настройки оператора.
    static QTemporaryDir settingsDir;
    static const bool configured = [] {
        QCoreApplication::setOrganizationName(QStringLiteral("AFAR-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("test_main_window"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QSettings().clear();
        return true;
    }();
    static_cast<void>(configured);
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

quint16 freeTlmPort()
{
    QUdpSocket probe;
    REQUIRE(probe.bind(QHostAddress::LocalHost, 0));
    const quint16 port = probe.localPort();
    probe.close();
    return port;
}

/// Главное окно как в программе: движок спектра не запущен, PS-сессия не сканирует сеть.
struct Window {
    Window()
    {
        applyAppTheme(*qApp, AppTheme::Light);
        QSettings().clear();
        engine = std::make_unique<SpectrumEngine>(false);
        window = std::make_unique<MainWindow>(*engine);
        session = window->findChild<PsSession*>();
        REQUIRE(session != nullptr);
        session->macResolver()->setArpReader([] { return QHash<QString, QString>{}; });
        session->macResolver()->setCandidateProvider([] { return QStringList{}; });
        tabs = window->findChild<QTabWidget*>(QStringLiteral("mainTabs"));
        REQUIRE(tabs != nullptr);
        window->resize(1360, 860);
        window->show();
        static_cast<void>(QTest::qWaitForWindowExposed(window.get()));
    }
    ~Window()
    {
        window.reset();
        engine.reset();
        QSettings().clear();
    }
    void openTab(int index)
    {
        tabs->setCurrentIndex(index);
        // Раскладка вкладки достраивается несколькими проходами цикла событий.
        for (int i = 0; i < 4; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
    }

    std::unique_ptr<SpectrumEngine> engine;
    std::unique_ptr<MainWindow> window;
    PsSession* session = nullptr;
    QTabWidget* tabs = nullptr;
};

/// Нашли ли мы на странице вкладки виджеты, которые налезают друг на друга или сжаты меньше своего минимума.
QStringList layoutProblems(QWidget* page)
{
    QStringList problems;
    const auto describe = [](const QWidget* w) {
        QString text = QString::fromLatin1(w->metaObject()->className());
        if (!w->objectName().isEmpty()) {
            text += QStringLiteral("#") + w->objectName();
        }
        if (const auto* b = qobject_cast<const QPushButton*>(w)) {
            text += QStringLiteral(" «%1»").arg(b->text());
        } else if (const auto* l = qobject_cast<const QLabel*>(w)) {
            text += QStringLiteral(" «%1»").arg(l->text().left(30));
        } else if (const auto* g = qobject_cast<const QGroupBox*>(w)) {
            text += QStringLiteral(" «%1»").arg(g->title());
        }
        return text;
    };

    QList<QWidget*> all{page};
    all += page->findChildren<QWidget*>();
    for (QWidget* parent : all) {
        if (!parent->isVisible()) {
            continue;
        }
        // 1. Налезание: соседи в одной раскладке не должны пересекаться.
        QLayout* layout = parent->layout();
        if (layout && !qobject_cast<QStackedLayout*>(layout)) {
            QList<QWidget*> kids;
            for (QObject* o : parent->children()) {
                auto* w = qobject_cast<QWidget*>(o);
                // Подложки строк таблиц полей («Модули») лежат под виджетами строки намеренно.
                if (w && w->isVisible() && !w->isWindow() &&
                    !w->property("role").toString().startsWith(QStringLiteral("grid"))) {
                    kids << w;
                }
            }
            for (int i = 0; i < kids.size(); ++i) {
                for (int j = i + 1; j < kids.size(); ++j) {
                    const QRect overlap = kids[i]->geometry().intersected(kids[j]->geometry());
                    if (overlap.width() > 2 && overlap.height() > 2) {
                        problems << QStringLiteral("налезают: %1 и %2 (%3×%4 px)")
                                        .arg(describe(kids[i]), describe(kids[j]))
                                        .arg(overlap.width())
                                        .arg(overlap.height());
                    }
                }
            }
        }
        // 2. Сжатие: виджет в раскладке не меньше своего минимума (прокручиваемые области сами прокручиваются).
        if (parent == page || qobject_cast<QAbstractScrollArea*>(parent) || qobject_cast<QTabBar*>(parent) ||
            qobject_cast<QTabWidget*>(parent)) {
            continue;
        }
        // Внутренности Qt (поле внутри счётчика и т. п.) и счётчики/списки по высоте не считаем: их
        // минимум под таблицей стилей Qt считает неточно, а на экране они в порядке.
        if (parent->objectName().startsWith(QStringLiteral("qt_"))) {
            continue;
        }
        const bool spinOrCombo = qobject_cast<QAbstractSpinBox*>(parent) || qobject_cast<QComboBox*>(parent);
        const QSize min = parent->minimumSizeHint();
        if (min.isValid() && (parent->width() + 2 < min.width() || (!spinOrCombo && parent->height() + 2 < min.height()))) {
            problems << QStringLiteral("сжат: %1 %2×%3, минимум %4×%5")
                            .arg(describe(parent))
                            .arg(parent->width())
                            .arg(parent->height())
                            .arg(min.width())
                            .arg(min.height());
        }
    }
    return problems;
}

}  // namespace

TEST_CASE("VNA probe button starts a hidden S2VNA and finishes without hanging", "[.s2vna-live-ui]")
{
    if (!qEnvironmentVariableIsSet("AFAR_TEST_LIVE_S2VNA")) {
        SKIP("Requires an explicitly reserved local S2VNA installation");
    }
    app();
    Window w;
    w.openTab(1);
    auto* backend = child<QComboBox>(*w.window, "vnaBackend");
    backend->setCurrentIndex(backend->findData(1));
    QPushButton* probe = nullptr;
    for (auto* button : w.window->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Вкл")) {
            probe = button;
            break;
        }
    }
    REQUIRE(probe != nullptr);
    QString warning;
    QTimer dismissWarning;
    dismissWarning.setInterval(100);
    QObject::connect(&dismissWarning, &QTimer::timeout, [&] {
        for (QWidget* topLevel : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QMessageBox*>(topLevel);
            if (dialog && dialog->isVisible()) {
                warning = dialog->text();
                dialog->accept();
            }
        }
    });
    dismissWarning.start();
    probe->click();
    const auto diagnostics = w.window->findChildren<QLabel*>();
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](QLabel* label) {
        return label->text().contains(QStringLiteral("S2VNA запускается скрыто"));
    }));
    const bool completed = waitFor([&] {
        return !warning.isEmpty() || std::any_of(diagnostics.begin(), diagnostics.end(), [](QLabel* label) {
                   return label->objectName() == QStringLiteral("dataSourceBadge")
                       && label->text().contains(QStringLiteral("24111049"));
               });
    }, 50000);
    REQUIRE(completed);
    dismissWarning.stop();
    INFO(warning.toStdString());
    REQUIRE(warning.isEmpty());
    CHECK(std::any_of(diagnostics.begin(), diagnostics.end(), [](QLabel* label) {
        return label->objectName() == QStringLiteral("dataSourceBadge")
            && label->text().contains(QStringLiteral("24111049"));
    }));
}

TEST_CASE("VNA switch connects, disconnects and reconnects the demo stand", "[ui][window]")
{
    app();
    Window w;
    w.openTab(1);
    auto* backend = child<QComboBox>(*w.window, "vnaBackend");
    backend->setCurrentIndex(backend->findData(0));
    QPushButton* power = nullptr;
    for (auto* button : w.window->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Вкл")) {
            power = button;
            break;
        }
    }
    REQUIRE(power != nullptr);
    for (int i = 0; i < 2; ++i) {
        power->click();
        REQUIRE(waitFor([&] {
            const auto labels = w.window->findChildren<QLabel*>();
            return power->text() == QStringLiteral("Выкл")
                && std::any_of(labels.begin(), labels.end(), [](QLabel* label) {
                       return label->text().startsWith(QStringLiteral("C2220:"))
                           && !label->text().contains(QStringLiteral("нет связи"));
                   });
        }));
        power->click();
        REQUIRE(waitFor([&] {
            const auto labels = w.window->findChildren<QLabel*>();
            return power->text() == QStringLiteral("Вкл") && backend->isEnabled()
                && std::any_of(labels.begin(), labels.end(), [](QLabel* label) {
                       return label->text().startsWith(QStringLiteral("C2220: нет связи"));
                   });
        }));
    }
}

TEST_CASE("Главное окно: пять вкладок в порядке эскиза", "[ui][window]")
{
    app();
    Window w;
    QStringList names;
    for (int i = 0; i < w.tabs->count(); ++i) {
        names << w.tabs->tabText(i);
    }
    CHECK(names == QStringList{QStringLiteral("Настройки"), QStringLiteral("Измерение S-параметров"), QStringLiteral("Спектр"),
                               QStringLiteral("Тестовый прогон"), QStringLiteral("Приёмка")});
    // Окно умещается на небольшом экране: содержимое вкладок прокручивается, а не растягивает окно.
    CHECK(w.window->minimumSizeHint().width() <= 920);
    CHECK(w.window->minimumSizeHint().height() <= 580);
}

TEST_CASE("Измерение S-параметров: управление находится справа в одной области с графиками",
          "[ui][window][measurement]")
{
    app();
    Window w;
    w.openTab(1);

    auto* plot = child<S2VnaGraphHost>(*w.window, "s2vnaGraphHost");
    auto* controls = child<QScrollArea>(*w.window, "graphControlsScroll");
    auto* connections = child<QWidget>(*w.window, "ConnectionBar");
    CHECK_FALSE(connections->isAncestorOf(controls));
    CHECK(controls->mapTo(w.window.get(), QPoint()).x() > plot->mapTo(w.window.get(), QPoint()).x());
}

TEST_CASE("Для отчёта остаётся разовый съём без собственной отрисовки графиков",
          "[ui][window][measurement]")
{
    app();
    Window w;
    w.openTab(1);
    auto* backend = child<QComboBox>(*w.window, "vnaBackend");
    backend->setCurrentIndex(backend->findData(0));
    QPushButton* power = nullptr;
    for (auto* candidate : w.window->findChildren<QPushButton*>()) {
        if (candidate->text() == QStringLiteral("Вкл")) {
            power = candidate;
            break;
        }
    }
    REQUIRE(power != nullptr);
    power->click();
    REQUIRE(waitFor([&] {
        for (auto* label : w.window->findChildren<QLabel*>()) {
            if (label->text().startsWith(QStringLiteral("C2220:"))
                && !label->text().contains(QStringLiteral("нет связи"))) return true;
        }
        return false;
    }));
    CHECK(w.window->findChild<QPushButton*>(QStringLiteral("buildGraphsButton")) == nullptr);
    CHECK(w.window->findChild<QWidget*>(QStringLiteral("graphPlot0")) == nullptr);
    auto* button = child<QPushButton>(*w.window, "captureForReport");
    REQUIRE(button->text() == QStringLiteral("Снять измерение для отчёта"));
    REQUIRE_FALSE(button->isCheckable());
    button->click();
    auto* exportButton = child<QPushButton>(*w.window, "exportTwoPortReport");
    REQUIRE(waitFor([&] { return exportButton->isEnabled(); }, 10000));
}

TEST_CASE("Настройка частоты открывается рядом с подключением и сворачивается кликом",
          "[ui][window][measurement]")
{
    app();
    Window w;
    w.openTab(1);
    auto* tabs = child<QTabWidget>(*w.window, "connectionTabs");
    auto* calibration = child<QTabWidget>(*w.window, "calibrationTabs");
    auto* frequency = child<QDoubleSpinBox>(*w.window, "frequencyStart");
    REQUIRE(tabs->count() == 5);
    CHECK(tabs->tabText(3) == QStringLiteral("Настройка частоты"));
    CHECK(tabs->tabText(4) == QStringLiteral("Настройка 1 канала"));
    CHECK(tabs->widget(4)->objectName() == QStringLiteral("singleChannelPanel"));
    CHECK(tabs->widget(3)->isAncestorOf(frequency));
    CHECK(calibration->indexOf(tabs->widget(3)) == -1);

    QPushButton* openSettings = nullptr;
    for (auto* button : w.window->findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("Настройки частоты, точек и ПЧ")) {
            openSettings = button;
            break;
        }
    }
    REQUIRE(openSettings != nullptr);
    openSettings->click();
    CHECK(tabs->currentIndex() == 3);
    CHECK(tabs->maximumHeight() == QWIDGETSIZE_MAX);
    auto* tabBar = tabs->tabBar();
    QMetaObject::invokeMethod(tabBar, "tabBarClicked", Qt::DirectConnection, Q_ARG(int, 3));
    CHECK(tabs->maximumHeight() <= tabBar->sizeHint().height() + 2);
}

TEST_CASE("Быстрая настройка канала на графиках синхронизирована с основной панелью",
          "[ui][window][measurement]")
{
    app();
    Window w;
    auto* channel = child<SingleChannelPanel>(*w.window, "singleChannelPanel");
    auto* quick = child<QGroupBox>(*w.window, "quickSingleChannelControls");
    auto* action = child<QGroupBox>(*w.window, "graphActionBox");
    auto* format = child<QComboBox>(*w.window, "graphDisplayFormat");
    auto* side = quick->parentWidget()->layout();
    REQUIRE(side != nullptr);
    CHECK(side->indexOf(quick) == side->indexOf(action) + 1);
    CHECK(side->indexOf(format->parentWidget()) == side->indexOf(quick) + 1);

    auto* phaseSlider = child<QSlider>(*w.window, "quickChannelPhaseSlider");
    auto* phaseValue = child<QDoubleSpinBox>(*w.window, "quickChannelPhaseValue");
    auto* attenSlider = child<QSlider>(*w.window, "quickChannelAttenSlider");
    auto* attenValue = child<QDoubleSpinBox>(*w.window, "quickChannelAttenValue");
    phaseSlider->setValue(8);
    CHECK(channel->phaseCode() == 8);
    CHECK(phaseValue->value() == 45.0);
    CHECK(child<QSpinBox>(*w.window, "singleChannelPhaseCode")->value() == 8);
    phaseValue->setValue(90.0);
    QMetaObject::invokeMethod(phaseValue, "editingFinished", Qt::DirectConnection);
    CHECK(channel->phaseCode() == 16);
    CHECK(phaseSlider->value() == 16);
    child<QPushButton>(*w.window, "quickChannelPhaseUp")->click();
    CHECK(channel->phaseCode() == 17);
    CHECK(phaseSlider->value() == 17);
    child<QPushButton>(*w.window, "quickChannelPhaseDown")->click();
    CHECK(channel->phaseCode() == 16);

    attenSlider->setValue(43);
    CHECK(channel->attenCode() == 43);
    CHECK(attenValue->value() == -10.0);
    attenValue->setValue(-5.0);
    QMetaObject::invokeMethod(attenValue, "editingFinished", Qt::DirectConnection);
    CHECK(channel->attenCode() == 53);
    CHECK(attenSlider->value() == 53);
    CHECK(child<QSpinBox>(*w.window, "singleChannelAttenCode")->value() == 53);
    child<QPushButton>(*w.window, "quickChannelAttenUp")->click();
    CHECK(channel->attenCode() == 54);
    CHECK(attenValue->value() == -4.5);
    child<QPushButton>(*w.window, "quickChannelAttenDown")->click();
    CHECK(channel->attenCode() == 53);
    CHECK(attenValue->value() == -5.0);
    channel->setPhaseCode(63);
    child<QPushButton>(*w.window, "quickChannelPhaseUp")->click();
    CHECK(channel->phaseCode() == 63);
    channel->setAttenCode(0);
    child<QPushButton>(*w.window, "quickChannelAttenDown")->click();
    CHECK(channel->attenCode() == 0);
}

TEST_CASE("График S2VNA возвращается при переходе на другую вкладку",
          "[ui][window][native][.]")
{
    app();
    if (qEnvironmentVariableIsEmpty("AFAR_TEST_S2VNA_DEMO")) {
        SKIP("требуется отдельно запущенная S2VNA в деморежиме");
    }
    Window w;
    w.openTab(1);
    auto* host = child<S2VnaGraphHost>(*w.window, "s2vnaGraphHost");
    auto* button = child<QPushButton>(*w.window, "showS2vnaGraph");
    auto* stack = child<QStackedWidget>(*w.window, "graphViewStack");
    QString error;
    const bool attached = waitFor([&] { return host->attach(&error); }, 10000);
    INFO(error.toStdString());
    REQUIRE(attached);
    CHECK(host->isAttached());
    host->detach();
    CHECK_FALSE(host->isAttached());
    QTimer::singleShot(3000, w.window.get(), [] {
        if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            dialog->accept();
        }
    });
    button->click();
    REQUIRE(host->isAttached());
    CHECK(stack->currentWidget() == host);
    const QString shot = qEnvironmentVariable("AFAR_TEST_S2VNA_SHOT");
    if (!shot.isEmpty()) {
        auto* workspace = child<QScrollArea>(*w.window, "sParametersWorkspace");
        workspace->verticalScrollBar()->setValue(workspace->verticalScrollBar()->maximum());
        QTest::qWait(300);
        REQUIRE(w.window->screen()->grabWindow(w.window->winId()).save(shot));
    }
    w.openTab(0);
    CHECK_FALSE(host->isAttached());
    CHECK_FALSE(button->isChecked());
    CHECK(stack->currentIndex() == 0);
}

TEST_CASE("S2VNA: четыре трассы настраиваются без дополнительных экранов",
          "[ui][window][native]")
{
    app();
    Window w;
    w.openTab(1);
    auto* measure = w.window->findChild<MeasureTab*>();
    REQUIRE(measure != nullptr);
    auto* button = child<QPushButton>(*w.window, "loadS2vnaState");
    CHECK(button->text().contains(QStringLiteral(".sta")));
    QStringList expected{QStringLiteral("S11"), QStringLiteral("S21"),
                         QStringLiteral("S12"), QStringLiteral("S22")};
    measure->setNativeGraphState(expected,
                                 {QStringLiteral("MLOG"), QStringLiteral("MLOG"),
                                  QStringLiteral("MLOG"), QStringLiteral("MLOG")}, 7);
    for (const auto& parameter : expected) {
        auto* trace = w.window->findChild<QPushButton*>(QStringLiteral("graphTrace") + parameter);
        REQUIRE(trace != nullptr);
        CHECK(trace->isChecked());
    }
    auto* together = child<QPushButton>(*w.window, "nativeTogetherButton");
    CHECK(together->text() == QStringLiteral("Вместе"));
    measure->setNativeStateLoading(true);
    CHECK_FALSE(button->isEnabled());
    measure->setNativeStateLoading(false);
    CHECK(button->isEnabled());
    auto* undo = child<QPushButton>(*w.window, "undoS2vnaState");
    CHECK_FALSE(undo->isEnabled());
    measure->setNativeStateUndoAvailable(true);
    CHECK(undo->isEnabled());
    measure->setNativeStateLoading(true);
    CHECK_FALSE(undo->isEnabled());
    measure->setNativeStateLoading(false);
    CHECK(undo->isEnabled());

    measure->setInstrumentSweepSettings(4.9e9, 6.0e9, 5501, 5000, -40.0, 1);
    CHECK(measure->fStartHz() == 4.9e9);
    CHECK(measure->fStopHz() == 6.0e9);
    CHECK(measure->points() == 5501);
    CHECK(measure->ifbwHz() == 5000);
    CHECK(measure->powerDbm() == -40.0);
    int applyRequested = 0;
    QObject::connect(measure, &MeasureTab::nativeSweepSettingsApplyRequested,
                     measure, [&applyRequested] { ++applyRequested; });
    child<QPushButton>(*w.window, "applyNativeSweepSettings")->click();
    CHECK(applyRequested == 1);
    measure->setNativeOperatingState(true, true);
    auto* rfOutput = child<QCheckBox>(*w.window, "nativeRfOutput");
    auto* correction = child<QCheckBox>(*w.window, "nativeCorrection");
    CHECK(rfOutput->isChecked());
    CHECK(correction->isChecked());
    int operatingRequested = 0;
    QObject::connect(measure, &MeasureTab::nativeOperatingStateApplyRequested,
                     measure, [&operatingRequested](bool rf, bool corr) {
                         CHECK_FALSE(rf);
                         CHECK(corr);
                         ++operatingRequested;
                     });
    rfOutput->setChecked(false);
    child<QPushButton>(*w.window, "applyNativeOperatingState")->click();
    CHECK(operatingRequested == 1);
    measure->setVnaCalibrationIdHint(QStringLiteral("CKIT 14 (example)"));
    CHECK(child<QSpinBox>(*w.window, "vnaCalibrationKit")->value() == 14);
}

TEST_CASE("Главное окно: «Тех. состояние» — только параметры платы, «Одна команда» без «Журнала»", "[ui][window]")
{
    app();
    Window w;
    w.openTab(0);
    auto* tech = child<QGroupBox>(*w.window, "techBox");
    auto* boardTabs = tech->findChild<QTabWidget*>(QStringLiteral("boardParametersTabs"));
    REQUIRE(boardTabs != nullptr);
    CHECK(boardTabs->tabText(0) == QStringLiteral("PS часть"));
    CHECK(boardTabs->tabText(1) == QStringLiteral("Ошибки"));
    CHECK(boardTabs->tabText(2) == QStringLiteral("Состояние"));
    CHECK(boardTabs->tabText(3) == QStringLiteral("Подробно"));
    // Трансивер AD9361, ПЛИС и РЧ-модули — в «Тех. состоянии», а не в «Модулях».
    CHECK(tech->findChild<QPushButton*>(QStringLiteral("psGetTrxStatus")) != nullptr);
    CHECK(tech->findChild<QPushButton*>(QStringLiteral("psGetPlStatus")) != nullptr);
    CHECK(tech->findChild<QPushButton*>(QStringLiteral("psGetRfmTelemetry")) != nullptr);
    CHECK(tech->findChild<QCheckBox*>(QStringLiteral("psPollTrx")) != nullptr);
    auto* modules = child<QGroupBox>(*w.window, "modulesBox");
    CHECK(modules->findChild<QPushButton*>(QStringLiteral("psGetTrxStatus")) == nullptr);
    // Телеметрия PS в «Тех. состоянии» не показывается: нет ни вкладки, ни плиток узлов.
    for (QTabWidget* tabs : tech->findChildren<QTabWidget*>()) {
        for (int i = 0; i < tabs->count(); ++i) {
            CHECK_FALSE(tabs->tabText(i).contains(QStringLiteral("елеметрия")));
        }
    }
    CHECK(tech->findChild<QLabel*>(QStringLiteral("psTechValues")) == nullptr);
    CHECK(tech->findChild<QLabel*>(QStringLiteral("psTechMode")) == nullptr);
    CHECK(tech->findChild<QPushButton*>(QStringLiteral("readBoardParameters")) != nullptr);

    auto* command = child<QWidget>(*w.window, "commandBox");
    CHECK(command->findChildren<QTabWidget*>().isEmpty());

    // Без связи с PS «Получить параметры» ничего не шлёт и говорит об этом.
    child<QPushButton>(*w.window, "readBoardParameters")->click();
    CHECK(child<QLabel>(*w.window, "boardParametersStatus")->text().contains(QStringLiteral("Нет связи")));
}

TEST_CASE("Главное окно: IP платы вводится с клавиатуры, подключение, ответы и журнал на разных вкладках",
          "[ui][window][input]")
{
    app();
    TestPsServer server;
    server.setTelemetryPeriodMs(100);
    REQUIRE(server.listen());
    Window w;
    w.openTab(0);

    auto* host = child<QLineEdit>(*w.window, "psHost");
    auto* mac = child<QLineEdit>(*w.window, "psMac");
    auto* port = child<QSpinBox>(*w.window, "psPort");
    auto* tlm = child<QSpinBox>(*w.window, "psTlmPort");
    // Поле можно набрать пальцами, как оператор: клик, затем клавиши.
    for (QLineEdit* edit : {host, mac}) {
        REQUIRE(edit->isEnabled());
        CHECK_FALSE(edit->isReadOnly());
    }
    QTest::mouseClick(host, Qt::LeftButton);
    host->selectAll();
    QTest::keyClicks(host, QStringLiteral("172.16.226.99"));
    CHECK(host->text() == QStringLiteral("172.16.226.99"));
    host->selectAll();
    QTest::keyClicks(host, QStringLiteral("127.0.0.1"));
    mac->clear();
    port->setValue(server.port());
    tlm->setValue(freeTlmPort());
    QTest::keyClick(host, Qt::Key_Return);  // Enter подключает
    REQUIRE(waitFor([&] { return w.session->link()->isReady(); }));

    auto* status = child<QLabel>(*w.window, "psLinkState");
    CHECK(status->text().startsWith(QStringLiteral("на связи")));
    CHECK(child<QPushButton>(*w.window, "psConnect")->text() == QStringLiteral("Отключиться"));
    CHECK(child<QWidget>(*w.window, "psTileTcpOut") != nullptr);
    // После подключения параметры платы запрашиваются сами, без нажатия «Получить параметры».
    auto* boardStatus = child<QLabel>(*w.window, "boardParametersStatus");
    CHECK(waitFor([&] { return boardStatus->text() == QStringLiteral("Параметры платы получены"); }, 4000));
    auto* boardMain = child<QPlainTextEdit>(*w.window, "boardParametersOutput");
    CHECK(boardMain->toPlainText().contains(QStringLiteral("Связь: 127.0.0.1")));
    CHECK(boardMain->toPlainText().contains(QStringLiteral("Режим:")));
    auto* boardDetails = child<QPlainTextEdit>(*w.window, "boardParametersDetails");
    CHECK(boardDetails->toPlainText().contains(QStringLiteral("Состояние изделия")));
    CHECK(w.window->statusBar() != nullptr);
    // Строка состояния внизу окна обновляется по таймеру.
    auto* moduleStatus = w.window->findChild<QLabel*>(QStringLiteral("moduleStatusText"));
    REQUIRE(moduleStatus != nullptr);
    CHECK(waitFor([&] { return moduleStatus->text().contains(QStringLiteral("PS: на связи")); }, 3000));

    // Одна и та же связь работает на других вкладках без повторного подключения.
    w.openTab(0);
    auto* toService = child<QPushButton>(*w.window, "psToService");
    REQUIRE(waitFor([&] { return toService->isEnabled(); }));
    toService->click();
    REQUIRE(waitFor([&] { return child<QPushButton>(*w.window, "psWriteCodes")->isEnabled(); }));
    CHECK(child<QLabel>(*w.window, "psMode")->text().contains(QStringLiteral("сервисный")));

    w.openTab(3);
    auto* journal = child<QTableWidget>(*w.window, "testRunJournal");
    CHECK(journal->rowCount() > 3);
    child<QPushButton>(*w.window, "udpCheckStart")->click();
    CHECK(child<QLabel>(*w.window, "udpVerdict")->text().startsWith(QStringLiteral("Идёт проверка")));
    REQUIRE(waitFor([&] { return !child<QLabel>(*w.window, "udpVerdict")->text().startsWith(QStringLiteral("Идёт")); },
                    14000));
    CHECK(child<QLabel>(*w.window, "udpVerdict")->text().startsWith(QStringLiteral("Норма")));

    w.openTab(4);
    auto* start = child<QPushButton>(*w.window, "psCheckStart");
    CHECK(start->isEnabled());

    // Отключение на вкладке «Настройки» гасит ручные команды везде.
    w.openTab(0);
    child<QPushButton>(*w.window, "psConnect")->click();
    CHECK_FALSE(w.session->link()->isReady());
    CHECK_FALSE(child<QPushButton>(*w.window, "psWriteCodes")->isEnabled());
    CHECK(host->isEnabled());
}

TEST_CASE("Спектр запрашивает SERVICE и адрес PS после открытия вкладки", "[ui][window][spectrum]")
{
    app();
    TestPsServer server;
    REQUIRE(server.listen());
    Window w;
    w.openTab(0);
    child<QLineEdit>(*w.window, "psHost")->setText(QStringLiteral("127.0.0.1"));
    child<QLineEdit>(*w.window, "psMac")->clear();
    child<QSpinBox>(*w.window, "psPort")->setValue(server.port());
    child<QSpinBox>(*w.window, "psTlmPort")->setValue(freeTlmPort());
    child<QPushButton>(*w.window, "psConnect")->click();
    REQUIRE(waitFor([&] { return w.session->link()->isReady() && w.session->modeKnown(); }));
    CHECK(server.mode() == afar::ps::Mode::Idle);
    CHECK(w.engine->state().ps_control_connected.load());
    CHECK_FALSE(w.engine->state().ps_service_mode.load());

    w.openTab(2);
    REQUIRE(waitFor([&] { return server.mode() == afar::ps::Mode::Service; }));
    REQUIRE(waitFor([&] { return w.engine->state().ps_service_mode.load(); }));
    REQUIRE(waitFor([&] { return w.engine->state().connect_requested.load(); }));
    {
        std::lock_guard<std::mutex> lock(w.engine->state().connection_mutex);
        CHECK(w.engine->state().pluto_ip == "ip:127.0.0.1");
    }

    // После первого успеха и последующего обрыва IQ вкладка должна переподключиться.
    auto* retryTimer = child<QTimer>(*w.window, "spectrumConnectTimer");
    retryTimer->setInterval(20);
    w.engine->state().connect_requested.store(false);
    w.engine->state().is_connected.store(true);
    REQUIRE(waitFor([&] { return retryTimer->isActive(); }));
    QTest::qWait(50);
    w.engine->state().is_connected.store(false);
    REQUIRE(waitFor([&] { return w.engine->state().connect_requested.load(); }));

    w.openTab(0);
    child<QPushButton>(*w.window, "psToIdle")->click();
    REQUIRE(waitFor([&] { return server.mode() == afar::ps::Mode::Idle; }));
    REQUIRE(waitFor([&] { return !w.engine->state().ps_service_mode.load(); }));
    CHECK(w.engine->state().ps_control_connected.load());
    CHECK(w.engine->state().disconnect_requested.load());
}

TEST_CASE("Спектр не сохраняет неизвестную частоту как нулевую", "[ui][spectrum]")
{
    app();
    QSettings().setValue(QStringLiteral("spectrum/center_freq_hz"), qulonglong{104'200'000});
    AppState state;
    {
        SpectrumOpenGLWidget widget(state);
        state.ps_iio_read_only.store(true);
        state.center_freq_hz.store(0);
    }
    CHECK(QSettings().value(QStringLiteral("spectrum/center_freq_hz")).toULongLong()
          == 104'200'000ULL);
    QSettings().remove(QStringLiteral("spectrum/center_freq_hz"));
}

TEST_CASE("Главное окно: кнопка справа от вкладок переключает светлую и тёмную тему", "[ui][window][theme]")
{
    app();
    Window w;
    auto* button = child<QPushButton>(*w.window, "btnThemeToggle");
    auto* spectrum = dynamic_cast<SpectrumOpenGLWidget*>(
        w.window->findChild<QOpenGLWidget*>(QStringLiteral("spectrumWorkspace")));
    // На платформе offscreen спектра (OpenGL) нет — его тема проверяется, только если он есть.
    CHECK(currentAppTheme() == AppTheme::Light);
    CHECK(button->text() == QStringLiteral("Тёмная тема"));
    if (spectrum) {
        CHECK(spectrum->lightTheme());
    }
    const QString light = qApp->styleSheet();

    button->click();
    CHECK(currentAppTheme() == AppTheme::Dark);
    CHECK(button->text() == QStringLiteral("Светлая тема"));
    if (spectrum) {
        CHECK_FALSE(spectrum->lightTheme());
    }
    CHECK(qApp->styleSheet() != light);
    CHECK(qApp->palette().color(QPalette::Window).lightness() < 80);
    // Тёмная тема запоминается для следующего запуска.
    CHECK(loadAppTheme() == AppTheme::Dark);

    button->click();
    CHECK(currentAppTheme() == AppTheme::Light);
    CHECK(qApp->styleSheet() == light);
    CHECK(qApp->palette().color(QPalette::Window).lightness() > 200);
    if (spectrum) {
        CHECK(spectrum->lightTheme());
    }
}

TEST_CASE("Главное окно: на всех вкладках ничего не налезает друг на друга и не сжато при любом размере окна",
          "[ui][window][layout]")
{
    app();
    Window w;
    const QList<QSize> sizes{{900, 560}, {1280, 680}, {1512, 900}, {1536, 800}, {1920, 1000}, {2560, 1380}};
    for (AppTheme theme : {AppTheme::Light, AppTheme::Dark}) {
        applyAppTheme(*qApp, theme);
        for (const QSize& size : sizes) {
            w.window->resize(size);
            for (int tab = 0; tab < w.tabs->count(); ++tab) {
                if (w.tabs->tabText(tab) == QStringLiteral("Спектр")) {
                    continue;  // OpenGL-вкладка, внутри только ImGui
                }
                w.openTab(tab);
                // Страница вкладки: содержимое прокручиваемой области либо вложенные вкладки приёмки.
                QWidget* page = w.tabs->widget(tab);
                if (auto* area = qobject_cast<QScrollArea*>(page)) {
                    page = area->widget();
                }
                const QStringList problems = layoutProblems(page);
                INFO("вкладка «" << w.tabs->tabText(tab).toStdString() << "», окно " << size.width() << "×"
                                 << size.height() << (theme == AppTheme::Dark ? ", тёмная тема" : ", светлая тема"));
                INFO(problems.join(QStringLiteral("\n")).toStdString());
                CHECK(problems.isEmpty());
            }
        }
    }
    applyAppTheme(*qApp, AppTheme::Light);
}

TEST_CASE("Снимки вкладок в обеих темах для проверки внешнего вида", "[ui][shots][.]")
{
    app();
    const QString dirPath = qEnvironmentVariable("AFAR_TEST_SHOTS_DIR");
    if (dirPath.isEmpty()) {
        SKIP("задайте AFAR_TEST_SHOTS_DIR");
    }
    QDir().mkpath(dirPath);
    TestPsServer server;
    server.setTelemetryPeriodMs(100);
    REQUIRE(server.listen());
    Window w;
    w.window->resize(1360, 900);
    w.openTab(0);
    child<QLineEdit>(*w.window, "psHost")->setText(QStringLiteral("127.0.0.1"));
    child<QSpinBox>(*w.window, "psPort")->setValue(server.port());
    child<QSpinBox>(*w.window, "psTlmPort")->setValue(freeTlmPort());
    child<QPushButton>(*w.window, "psConnect")->click();
    REQUIRE(waitFor([&] { return w.session->link()->isReady(); }));

    // Набираем состояния, как их видит оператор: журнал, результаты сканирования, стресс-тест, приёмка.
    child<QPushButton>(*w.window, "sffStart")->click();
    REQUIRE(waitFor([&] { return child<QTableWidget>(*w.window, "sffResults")->rowCount() == 3; }));
    w.openTab(3);
    child<QSpinBox>(*w.window, "stressDuration")->setValue(2);
    child<QSpinBox>(*w.window, "stressCount")->setValue(60);
    child<QPushButton>(*w.window, "stressStart")->click();
    waitFor([&] { return false; }, 2500);
    child<QPushButton>(*w.window, "udpCheckStart")->click();
    waitFor([&] { return false; }, 3500);
    w.openTab(4);
    child<QPushButton>(*w.window, "psCheckStart")->click();
    waitFor([&] { return false; }, 2500);

    for (AppTheme theme : {AppTheme::Light, AppTheme::Dark}) {
        applyAppTheme(*qApp, theme);
        const QString suffix = theme == AppTheme::Dark ? QStringLiteral("dark") : QStringLiteral("light");
        for (int tab = 0; tab < w.tabs->count(); ++tab) {
            if (w.tabs->tabText(tab) == QStringLiteral("Спектр")) {
                continue;
            }
            w.openTab(tab);
            w.window->grab().save(QDir(dirPath).filePath(QStringLiteral("tab-%1-%2.png").arg(tab + 1).arg(suffix)));
        }
    }
    applyAppTheme(*qApp, AppTheme::Light);
}


TEST_CASE("Настройка 1 канала: совместный перебор и применение состояния стенда",
          "[ui][window][single_channel]")
{
    app();
    Window w;
    w.openTab(1);
    auto* panel = w.window->findChild<SingleChannelPanel*>();
    REQUIRE(panel != nullptr);
    CHECK(child<QPushButton>(*w.window, "singleChannelSaveState") != nullptr);
    CHECK(child<QPushButton>(*w.window, "singleChannelLoadState") != nullptr);

    auto* mode = child<QComboBox>(*w.window, "singleChannelSweepMode");
    REQUIRE(mode->count() == 4);
    auto* attenFrom = child<QSpinBox>(*w.window, "singleChannelSweepAttenFrom");
    auto* phaseFrom = child<QSpinBox>(*w.window, "singleChannelSweepPhaseFrom");
    mode->setCurrentIndex(0);
    CHECK(attenFrom->parentWidget()->isHidden());
    mode->setCurrentIndex(2);
    CHECK_FALSE(attenFrom->parentWidget()->isHidden());
    CHECK_FALSE(phaseFrom->parentWidget()->isHidden());
    CHECK(panel->channelState().sweep_mode == afar::ppkh4::SweepMode::Grid);

    auto st = panel->channelState();
    st.phase_code = 5;
    st.atten_code = 43;
    st.sweep_mode = afar::ppkh4::SweepMode::Together;
    st.sweep_dwell_ms = 700;
    panel->applyChannelState(st);
    CHECK(panel->sendCommand() == QStringLiteral("SEND 12 AC5"));
    CHECK(mode->currentIndex() == 3);
    const auto back = panel->channelState();
    CHECK(back.sweep_dwell_ms == 700);
    CHECK(back.phase_code == 5);
    CHECK(back.atten_code == 43);
}
