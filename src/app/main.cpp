#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
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

    // S2VNA всегда запускает и настраивает оператор; приложение — только SCPI-клиент.
    int result = 0;
    {
        MainWindow window;
        window.show();
        result = app.exec();
    }
#ifdef _WIN32
    CloseHandle(instanceMutex);
#endif
    return result;
}
