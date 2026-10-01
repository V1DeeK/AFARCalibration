#include "S2VnaRuntime.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

#include <algorithm>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>

#include <vector>
#endif

namespace S2VnaRuntime {
namespace {

#ifdef Q_OS_WIN
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
#endif

}  // namespace

QString defaultExecutablePath()
{
    return QStringLiteral("C:/VNA/S2VNA/S2VNA.exe");
}

bool enableSocketServer(const QString& executablePath, QString* error)
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
    setup.sync();
    if (setup.status() != QSettings::NoError
        || setup.value(QStringLiteral("SocketSvrSetup/SocketSvrEnabled")).toInt() != 1) {
        if (error) {
            *error = QStringLiteral("Не удалось включить Socket Server в %1").arg(setupPath);
        }
        return false;
    }
    return true;
}

StartResult ensureRunningHidden(const QString& executablePath)
{
    QString error;
    if (!enableSocketServer(executablePath, &error)) {
        return {false, false, error};
    }

#ifdef Q_OS_WIN
    if (!runningProcessIds().empty()) {
        hideRunningWindows();
        return {true, false, QStringLiteral("S2VNA уже запущена; Socket Server включён")};
    }

    const QFileInfo executable(executablePath);
    std::wstring application =
        QDir::toNativeSeparators(executable.absoluteFilePath()).toStdWString();
    std::wstring command = L"\"" + application + L"\"";
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
    CloseHandle(process.hProcess);
    return {true, true, QStringLiteral("S2VNA запущена скрыто; ожидается Socket Server")};
#else
    return {false, false, QStringLiteral("Автозапуск S2VNA поддерживается только в Windows")};
#endif
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
