#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "spectrum_engine.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#ifndef _WIN32
#include <QDir>
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

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("AFAR RX Calibration Studio"));
    app.setOrganizationName(QStringLiteral("AFAR"));

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
        // Снимки всех вкладок для документации: AFAR_TEST_SCREENSHOTS=<каталог>.
        const QString shotsDir = qEnvironmentVariable("AFAR_TEST_SCREENSHOTS");
        if (!shotsDir.isEmpty()) {
            QTimer::singleShot(1500, &window, [&app, &window, shotsDir] {
                auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("mainTabs"));
                QDir().mkpath(shotsDir);
                for (int i = 0; tabs && i < tabs->count(); ++i) {
                    tabs->setCurrentIndex(i);
                    QCoreApplication::processEvents();
                    window.grab().save(QDir(shotsDir).filePath(QStringLiteral("tab-%1.png").arg(i + 1)));
                }
                app.exit(tabs ? 0 : 3);
            });
        }
        result = app.exec();
        spectrumEngine.stopAndJoin();
    }
#ifdef _WIN32
    CloseHandle(instanceMutex);
#endif
    return result;
}
