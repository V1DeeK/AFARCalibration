// Проверки главного окна целиком — тем, что видит и делает оператор: вкладки, ввод с клавиатуры,
// подключение к PS, темы, перекрытие и сжатие виджетов на разных размерах окна.
// Снимки вкладок для проверки внешнего вида: AFAR_TEST_SHOTS_DIR=<каталог> test_main_window "[shots]".

#include <catch2/catch_test_macros.hpp>

#include "ConnectionTab.h"
#include "MacResolver.h"
#include "MainWindow.h"
#include "PsSession.h"
#include "TestPsServer.h"
#include "Theme.h"
#include "UiStyle.h"
#include "spectrum_engine.hpp"
#include "spectrum_opengl_widget.hpp"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QOpenGLWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedLayout>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QtTest/QtTest>

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
                if (w && w->isVisible() && !w->isWindow()) {
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

TEST_CASE("Главное окно: шесть вкладок в порядке эскиза", "[ui][window]")
{
    app();
    Window w;
    QStringList names;
    for (int i = 0; i < w.tabs->count(); ++i) {
        names << w.tabs->tabText(i);
    }
    CHECK(names == QStringList{QStringLiteral("Подключение"), QStringLiteral("Установка параметров"),
                               QStringLiteral("Измерение S-параметров"), QStringLiteral("Спектр"),
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
    w.openTab(2);

    auto* plot = child<QWidget>(*w.window, "graphPlot0");
    auto* controls = child<QScrollArea>(*w.window, "graphControlsScroll");
    auto* connections = child<QWidget>(*w.window, "ConnectionBar");
    CHECK_FALSE(connections->isAncestorOf(controls));
    CHECK(controls->mapTo(w.window.get(), QPoint()).x() > plot->mapTo(w.window.get(), QPoint()).x());
}

TEST_CASE("Главное окно: «Тех. состояние» — только параметры платы, «Одна команда» без «Журнала»", "[ui][window]")
{
    app();
    Window w;
    w.openTab(0);
    auto* tech = child<QGroupBox>(*w.window, "techBox");
    auto* boardTabs = tech->findChild<QTabWidget*>(QStringLiteral("boardParametersTabs"));
    REQUIRE(boardTabs != nullptr);
    CHECK(boardTabs->tabText(0) == QStringLiteral("Основное"));
    CHECK(boardTabs->tabText(1) == QStringLiteral("Подробно"));
    // Телеметрия PS в «Тех. состоянии» не показывается: нет ни вкладки, ни плиток узлов.
    for (QTabWidget* tabs : tech->findChildren<QTabWidget*>()) {
        for (int i = 0; i < tabs->count(); ++i) {
            CHECK_FALSE(tabs->tabText(i).contains(QStringLiteral("елеметрия")));
        }
    }
    CHECK(tech->findChild<QLabel*>(QStringLiteral("psTechValues")) == nullptr);
    CHECK(tech->findChild<QLabel*>(QStringLiteral("psTechMode")) == nullptr);
    CHECK(tech->findChild<QPushButton*>(QStringLiteral("readBoardParameters")) != nullptr);

    auto* command = child<QGroupBox>(*w.window, "commandBox");
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
    w.openTab(1);
    auto* toService = child<QPushButton>(*w.window, "psToService");
    REQUIRE(waitFor([&] { return toService->isEnabled(); }));
    toService->click();
    REQUIRE(waitFor([&] { return child<QPushButton>(*w.window, "psWriteCodes")->isEnabled(); }));
    CHECK(child<QLabel>(*w.window, "psMode")->text().contains(QStringLiteral("сервисный")));

    w.openTab(4);
    auto* journal = child<QTableWidget>(*w.window, "testRunJournal");
    CHECK(journal->rowCount() > 3);
    child<QPushButton>(*w.window, "udpCheckStart")->click();
    CHECK(child<QLabel>(*w.window, "udpVerdict")->text().startsWith(QStringLiteral("Идёт проверка")));
    REQUIRE(waitFor([&] { return !child<QLabel>(*w.window, "udpVerdict")->text().startsWith(QStringLiteral("Идёт")); },
                    14000));
    CHECK(child<QLabel>(*w.window, "udpVerdict")->text().startsWith(QStringLiteral("Норма")));

    w.openTab(5);
    auto* start = child<QPushButton>(*w.window, "psCheckStart");
    CHECK(start->isEnabled());

    // Отключение на вкладке «Подключение» гасит ручные команды везде.
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

    w.openTab(3);
    REQUIRE(waitFor([&] { return server.mode() == afar::ps::Mode::Service; }));
    REQUIRE(waitFor([&] { return w.engine->state().connect_requested.load(); }));
    {
        std::lock_guard<std::mutex> lock(w.engine->state().connection_mutex);
        CHECK(w.engine->state().pluto_ip == "ip:127.0.0.1");
    }

    w.openTab(1);
    child<QPushButton>(*w.window, "psToIdle")->click();
    REQUIRE(waitFor([&] { return server.mode() == afar::ps::Mode::Idle; }));
    CHECK(w.engine->state().disconnect_requested.load());
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
    w.openTab(4);
    child<QSpinBox>(*w.window, "stressDuration")->setValue(2);
    child<QSpinBox>(*w.window, "stressCount")->setValue(60);
    child<QPushButton>(*w.window, "stressStart")->click();
    waitFor([&] { return false; }, 2500);
    child<QPushButton>(*w.window, "udpCheckStart")->click();
    waitFor([&] { return false; }, 3500);
    w.openTab(5);
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
