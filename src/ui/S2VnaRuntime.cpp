#include "S2VnaRuntime.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <optional>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>

#include <vector>
#endif

namespace S2VnaRuntime {
namespace {

#ifdef Q_OS_WIN
HANDLE startedProcess = nullptr;
DWORD startedProcessId = 0;
std::optional<bool> startedDemoMode;

void clearStartedProcess()
{
    if (startedProcess) {
        CloseHandle(startedProcess);
        startedProcess = nullptr;
    }
    startedProcessId = 0;
    startedDemoMode.reset();
}

std::vector<DWORD> runningProcessIds()
{
    std::vector<DWORD> ids;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return ids;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"S2VNA.exe") == 0) {
                ids.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return ids;
}

BOOL CALLBACK hideWindow(HWND window, LPARAM context)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const auto* ids = reinterpret_cast<const std::vector<DWORD>*>(context);
    if (ids && std::find(ids->begin(), ids->end(), pid) != ids->end()) {
        ShowWindowAsync(window, SW_HIDE);
    }
    return TRUE;
}

BOOL CALLBACK closeStartedProcessWindow(HWND window, LPARAM context)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == static_cast<DWORD>(context)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}
#endif

bool configureSetup(const QString& executablePath, bool demoMode, QString* error)
{
    const QFileInfo executable(executablePath);
    if (!executable.isFile()) {
        if (error) {
            *error = QStringLiteral("S2VNA.exe не найден: %1").arg(executablePath);
        }
        return false;
    }

    const QString setupPath = executable.dir().filePath(QStringLiteral("System/Setup.dat"));
    QSettings setup(setupPath, QSettings::IniFormat);
    setup.setValue(QStringLiteral("SocketSvrSetup/SocketSvrEnabled"), 1);
    if (demoMode) {
        // S2VNA 26.x: DeviceID 23 — C2220; без прибора приложение работает в Demo Mode.
        setup.setValue(QStringLiteral("Hardware/DeviceID"), 23);
    }
    setup.sync();
    const bool socketEnabled =
        setup.value(QStringLiteral("SocketSvrSetup/SocketSvrEnabled")).toInt() == 1;
    const bool modelSelected =
        !demoMode || setup.value(QStringLiteral("Hardware/DeviceID")).toInt() == 23;
    if (setup.status() != QSettings::NoError || !socketEnabled || !modelSelected) {
        if (error) {
            *error = QStringLiteral("Не удалось настроить S2VNA в %1").arg(setupPath);
        }
        return false;
    }
    return true;
}

}  // namespace

QString defaultExecutablePath()
{
    return QStringLiteral("C:/VNA/S2VNA/S2VNA.exe");
}

bool enableSocketServer(const QString& executablePath, QString* error)
{
    return configureSetup(executablePath, false, error);
}

bool enableC2220DemoMode(const QString& executablePath, QString* error)
{
    return configureSetup(executablePath, true, error);
}

StartResult ensureRunningHidden(const QString& executablePath, bool demoMode)
{
#ifdef Q_OS_WIN
    if (startedProcess && WaitForSingleObject(startedProcess, 0) == WAIT_OBJECT_0) {
        clearStartedProcess();
    }
    if (startedProcess && startedDemoMode && *startedDemoMode != demoMode) {
        QString stopError;
        if (!stopStartedProcess(&stopError)) {
            return {false, false, stopError};
        }
    }
    const auto running = runningProcessIds();
    if (demoMode && !startedProcess && !running.empty()) {
        return {false, false,
                QStringLiteral("S2VNA уже запущена не из AFAR. Закройте её и повторите "
                               "подключение Demo Mode — смена режима требует перезапуска.")};
    }
#endif

    QString error;
    if (!configureSetup(executablePath, demoMode, &error)) {
        return {false, false, error};
    }

#ifdef Q_OS_WIN
    if (!runningProcessIds().empty()) {
        hideRunningWindows();
        return {true, false,
                demoMode
                    ? QStringLiteral("S2VNA Demo C2220 уже запущена")
                    : QStringLiteral("S2VNA уже запущена; Socket Server включён")};
    }

    const QFileInfo executable(executablePath);
    std::wstring application =
        QDir::toNativeSeparators(executable.absoluteFilePath()).toStdWString();
    std::wstring command =
        L"\"" + application + L"\" /SocketServer:on /SocketPort:5025 /visible:off";
    std::wstring workDir = QDir::toNativeSeparators(executable.absolutePath()).toStdWString();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        application.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT, nullptr, workDir.c_str(), &startup,
        &process);
    if (!started) {
        return {false, false,
                QStringLiteral("Не удалось запустить S2VNA (Windows error %1)")
                    .arg(GetLastError())};
    }
    CloseHandle(process.hThread);
    clearStartedProcess();
    startedProcess = process.hProcess;
    startedProcessId = process.dwProcessId;
    startedDemoMode = demoMode;
    return {true, true,
            demoMode
                ? QStringLiteral("S2VNA Demo C2220 запущена скрыто; ожидается Socket Server")
                : QStringLiteral("S2VNA запущена скрыто; ожидается Socket Server")};
#else
    return {false, false, QStringLiteral("Автозапуск S2VNA поддерживается только в Windows")};
#endif
}

bool stopStartedProcess(QString* error)
{
    if (error) {
        error->clear();
    }
#ifdef Q_OS_WIN
    if (!startedProcess) {
        return true;
    }

    const HANDLE process = startedProcess;
    const DWORD processId = startedProcessId;
    EnumWindows(closeStartedProcessWindow, static_cast<LPARAM>(processId));

    DWORD waitResult = WaitForSingleObject(process, 3000);
    if (waitResult == WAIT_TIMEOUT) {
        if (!TerminateProcess(process, 0)) {
            if (error) {
                *error = QStringLiteral("Не удалось закрыть запущенную AFAR программу S2VNA "
                                        "(Windows error %1)")
                             .arg(GetLastError());
            }
            clearStartedProcess();
            return false;
        }
        waitResult = WaitForSingleObject(process, 3000);
    }

    if (waitResult != WAIT_OBJECT_0) {
        if (error) {
            *error = QStringLiteral("S2VNA не завершилась (Windows error %1)")
                         .arg(GetLastError());
        }
        clearStartedProcess();
        return false;
    }
    clearStartedProcess();
#endif
    return true;
}

void hideRunningWindows()
{
#ifdef Q_OS_WIN
    const auto ids = runningProcessIds();
    if (!ids.empty()) {
        EnumWindows(hideWindow, reinterpret_cast<LPARAM>(&ids));
    }
#endif
}

}  // namespace S2VnaRuntime
