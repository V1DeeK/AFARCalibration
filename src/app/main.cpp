#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "spectrum_engine.hpp"
#include "spectrum_opengl_widget.hpp"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#ifndef _WIN32
#include <QLockFile>
#endif
#ifndef _WIN32
#include <QMessageBox>
#include <QStandardPaths>
#else
#include <qt_windows.h>
#endif

int main(int argc, char* argv[])
{
#ifdef _WIN32
    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\AFAR_RX_Calibration_Studio");
    const bool alreadyRunning = instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    if (instanceMutex == nullptr || alreadyRunning) {
        if (instanceMutex != nullptr) {
            CloseHandle(instanceMutex);
        }
        if (!qEnvironmentVariableIsSet("AFAR_DISABLE_AUTO_S2VNA")) {
            MessageBoxW(nullptr, L"Программа уже запущена.", L"AFAR RX Calibration Studio",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        }
        return 0;
    }
#endif

    // Формат OpenGL задаётся до QApplication: иначе на macOS вкладка «Спектр» чёрная.
    QSurfaceFormat::setDefaultFormat(SpectrumOpenGLWidget::surfaceFormat());

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("AFAR RX Calibration Studio"));
    app.setOrganizationName(QStringLiteral("AFAR"));
    // Автоматические прогоны не должны читать и менять настройки оператора.
    if (const QString settingsDir = qEnvironmentVariable("AFAR_TEST_SETTINGS_DIR"); !settingsDir.isEmpty()) {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir);
    }

#ifndef _WIN32
    QLockFile instanceLock(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                               .filePath(QStringLiteral("AFAR-RX-Calibration-Studio.lock")));
    if (!instanceLock.tryLock()) {
        if (!qEnvironmentVariableIsSet("AFAR_DISABLE_AUTO_S2VNA")) {
            QMessageBox::information(nullptr, QStringLiteral("AFAR RX Calibration Studio"),
                                     QStringLiteral("Программа уже запущена."));
        }
        return 0;
    }
#endif

    if (const QString theme = qEnvironmentVariable("AFAR_TEST_THEME"); !theme.isEmpty()) {
        saveAppTheme(theme == QLatin1String("dark") ? AppTheme::Dark : AppTheme::Light);
    }
    applyAppTheme(app, loadAppTheme());

    bool autoExitValid = false;
    const int autoExitMs = qEnvironmentVariableIntValue("AFAR_TEST_AUTO_EXIT_MS", &autoExitValid);
    if (autoExitValid && autoExitMs > 0)
        QTimer::singleShot(autoExitMs, &app, &QCoreApplication::quit);

    // S2VNA всегда запускает и настраивает оператор; приложение — только SCPI-клиент.
    int result = 0;
    {
        SpectrumEngine spectrumEngine;
        MainWindow window(spectrumEngine);
        spectrumEngine.start();
        window.show();

        bool switchCountValid = false;
        const int switchCount = qEnvironmentVariableIntValue(
            "AFAR_TEST_SWITCH_COUNT", &switchCountValid);
        if (switchCountValid && switchCount > 0) {
            auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("mainTabs"));
            if (!tabs || tabs->count() != 6) {
                QTimer::singleShot(0, &app, [&app] { app.exit(3); });
            } else {
                const int originalTab = tabs->currentIndex();
                auto* switchTimer = new QTimer(&window);
                switchTimer->setInterval(1);
                QObject::connect(switchTimer, &QTimer::timeout, &window,
                        [&, switchTimer, tabs, originalTab,
                         completed = 0, maxSwitchNs = qint64{0}]() mutable {
                    QElapsedTimer elapsed;
                    elapsed.start();
                    tabs->setCurrentIndex(completed % tabs->count());
                    maxSwitchNs = qMax(maxSwitchNs, elapsed.nsecsElapsed());
                    ++completed;
                    if (completed < switchCount)
                        return;

                    switchTimer->stop();
                    tabs->setCurrentIndex(originalTab);
                    app.exit(maxSwitchNs <= 200'000'000 ? 0 : 4);
                });
                switchTimer->start();
            }
        }
        // Снимки для документации и проверки: AFAR_TEST_SCREENSHOTS=<каталог> — по вкладкам,
        // как их рисует окно; AFAR_TEST_WINDOW_SIZE=ШxВ — размер окна.
        // Снимок берётся у системы (QScreen::grabWindow), то есть как видит оператор,
        // включая OpenGL-вкладку «Спектр».
        if (const QString size = qEnvironmentVariable("AFAR_TEST_WINDOW_SIZE"); !size.isEmpty()) {
            const QStringList wh = size.split(QLatin1Char('x'));
            if (wh.size() == 2)
                window.resize(wh.at(0).toInt(), wh.at(1).toInt());
        }
        if (qEnvironmentVariableIsSet("AFAR_TEST_DUMP_SIZES")) {
            auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("mainTabs"));
            QTimer::singleShot(800, &app, [&app, &window, tabs] {
                QTextStream out(stdout);
                out << "window " << window.width() << "x" << window.height() << " min "
                    << window.minimumSizeHint().width() << "x" << window.minimumSizeHint().height() << "\n";
                for (int i = 0; tabs && i < tabs->count(); ++i) {
                    const QWidget* page = tabs->widget(i);
                    out << "tab " << i << " " << tabs->tabText(i) << " min " << page->minimumSizeHint().width()
                        << "x" << page->minimumSizeHint().height() << " hint " << page->sizeHint().width() << "x"
                        << page->sizeHint().height();
                    if (const auto* area = qobject_cast<const QScrollArea*>(page); area && area->widget()) {
                        out << " content-min " << area->widget()->minimumSizeHint().width() << "x"
                            << area->widget()->minimumSizeHint().height();
                    }
                    out << "\n";
                }
                out.flush();
                app.exit(0);
            });
        }
        const QString shotsDir = qEnvironmentVariable("AFAR_TEST_SCREENSHOTS");
        if (!shotsDir.isEmpty()) {
            auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("mainTabs"));
            // AFAR_TEST_SHOT_TABS=3,5 — только эти вкладки (с нуля); по умолчанию все.
            QList<int> order;
            for (const QString& part : qEnvironmentVariable("AFAR_TEST_SHOT_TABS").split(QLatin1Char(','), Qt::SkipEmptyParts))
                order.append(part.toInt());
            if (order.isEmpty() && tabs) {
                for (int i = 0; i < tabs->count(); ++i)
                    order.append(i);
            }
            auto* shotTimer = new QTimer(&window);
            shotTimer->setInterval(900);
            QDir().mkpath(shotsDir);
            QObject::connect(shotTimer, &QTimer::timeout, &window,
                             [&app, &window, tabs, shotTimer, shotsDir, order, step = -1]() mutable {
                if (!tabs) {
                    app.exit(3);
                    return;
                }
                if (step >= 0) {
                    const QPixmap shot = window.screen()->grabWindow(window.winId());
                    shot.save(QDir(shotsDir).filePath(QStringLiteral("tab-%1.png").arg(order.at(step) + 1)));
                }
                ++step;
                if (step >= order.size()) {
                    shotTimer->stop();
                    app.exit(0);
                    return;
                }
                if (tabs->currentIndex() != order.at(step))
                    tabs->setCurrentIndex(order.at(step));
            });
            QTimer::singleShot(1500, shotTimer, qOverload<>(&QTimer::start));
        }
        result = app.exec();
        spectrumEngine.stopAndJoin();
    }
#ifdef _WIN32
    CloseHandle(instanceMutex);
#endif
    return result;
}
