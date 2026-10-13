#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "spectrum_engine.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSettings>
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
            auto* parametersButton = window.findChild<QPushButton*>(
                QStringLiteral("btnParametersWorkspace"));
            auto* afarButton = window.findChild<QPushButton*>(QStringLiteral("btnAfarWorkspace"));
            auto* spectrumButton = window.findChild<QPushButton*>(
                QStringLiteral("btnSpectrumWorkspace"));
            if (!parametersButton || !afarButton || !spectrumButton) {
                QTimer::singleShot(0, &app, [&app] { app.exit(3); });
            } else {
                QSettings settings;
                const QString workspaceKey = QStringLiteral("ui/active_workspace_v2");
                const int originalWorkspace = settings.contains(workspaceKey)
                    ? qBound(0, settings.value(workspaceKey, 1).toInt(), 2)
                    : qBound(0, settings.value(QStringLiteral("ui/active_workspace"), 0).toInt(), 1) + 1;
                const QVector<QPushButton*> workspaceButtons{
                    parametersButton, afarButton, spectrumButton};
                auto* switchTimer = new QTimer(&window);
                switchTimer->setInterval(1);
                QObject::connect(switchTimer, &QTimer::timeout, &window,
                        [&, switchTimer, workspaceButtons, originalWorkspace,
                         completed = 0, maxSwitchNs = qint64{0}]() mutable {
                    QElapsedTimer elapsed;
                    elapsed.start();
                    workspaceButtons.at(completed % workspaceButtons.size())->click();
                    maxSwitchNs = qMax(maxSwitchNs, elapsed.nsecsElapsed());
                    ++completed;
                    if (completed < switchCount)
                        return;

                    switchTimer->stop();
                    workspaceButtons.at(originalWorkspace)->click();
                    app.exit(maxSwitchNs <= 200'000'000 ? 0 : 4);
                });
                switchTimer->start();
            }
        }
        result = app.exec();
        spectrumEngine.stopAndJoin();
    }
#ifdef _WIN32
    CloseHandle(instanceMutex);
#endif
    return result;
}
