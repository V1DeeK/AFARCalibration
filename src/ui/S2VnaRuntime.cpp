#include "S2VnaRuntime.h"

#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>

#include <optional>
#include <string>

namespace {

std::optional<bool> anotherS2VnaRunning()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"S2VNA.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

QString installedExecutable()
{
    QSettings installed(
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QSettings::NativeFormat);
    for (const QString& key : installed.childGroups()) {
        installed.beginGroup(key);
        const QString name = installed.value(QStringLiteral("DisplayName")).toString();
        const QString location = installed.value(QStringLiteral("InstallLocation")).toString();
        installed.endGroup();
        const QString executable = QDir(location).filePath(QStringLiteral("S2VNA.exe"));
        if (name.startsWith(QStringLiteral("S2VNA"), Qt::CaseInsensitive)
            && QFileInfo::exists(executable)) {
            return executable;
        }
    }
    const QString usualPath = QStringLiteral("C:/VNA/S2VNA/S2VNA.exe");
    if (QFileInfo::exists(usualPath)) {
        return usualPath;
    }
    return QStandardPaths::findExecutable(QStringLiteral("S2VNA.exe"));
}

BOOL CALLBACK requestClose(HWND window, LPARAM processId)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(processId)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

bool socketPortFree(int port)
{
    QTcpServer socket;
    return socket.listen(QHostAddress::LocalHost, static_cast<quint16>(port));
}

}  // namespace
#endif

S2VnaRuntime::~S2VnaRuntime()
{
#ifdef Q_OS_WIN
    stopOwned();
#endif
}

void S2VnaRuntime::stopOwned()
{
#ifdef Q_OS_WIN
    if (m_process) {
        if (WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT) {
            EnumWindows(requestClose, static_cast<LPARAM>(m_processId));
            WaitForSingleObject(static_cast<HANDLE>(m_process), 3000);
        }
    }
    // Job закрывает только запущенный нами экземпляр, даже после аварийного выхода AFAR.
    if (m_job) {
        CloseHandle(static_cast<HANDLE>(m_job));
    }
    if (m_process) {
        WaitForSingleObject(static_cast<HANDLE>(m_process), 3000);
        CloseHandle(static_cast<HANDLE>(m_process));
    }
    m_job = nullptr;
    m_process = nullptr;
    m_processId = 0;
    m_socketPort = 0;
#endif
}

bool S2VnaRuntime::ownedProcessRunning() const
{
#ifdef Q_OS_WIN
    return m_process && WaitForSingleObject(static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT;
#else
    return false;
#endif
}

S2VnaRuntime::StartResult S2VnaRuntime::ensureRunningHidden(int socketPort)
{
#ifdef Q_OS_WIN
    if (socketPort < 1 || socketPort > 65535) {
        return {false, false, QStringLiteral("Некорректный порт S2VNA")};
    }
    if (ownedProcessRunning() && socketPort == m_socketPort) {
        return {true, false, QStringLiteral("Ранее запущенная S2VNA ещё работает")};
    }
    if (ownedProcessRunning() && !socketPortFree(socketPort)) {
        return {false, false, QStringLiteral("Порт %1 занят; S2VNA не перезапущена")
                                  .arg(socketPort)};
    }
    if (m_process) {
        // При смене порта или завершении процесса сначала убираем весь наш Job.
        stopOwned();
    }
    const auto running = anotherS2VnaRunning();
    if (!running) {
        return {false, false, QStringLiteral("Не удалось проверить запущенные процессы S2VNA")};
    }
    if (*running) {
        return {true, false, QStringLiteral("Уже запущенная S2VNA будет использована без изменений")};
    }
    if (!socketPortFree(socketPort)) {
        return {false, false, QStringLiteral("Порт %1 занят другой программой; S2VNA не запущена")
                                  .arg(socketPort)};
    }
    const QString executable = installedExecutable();
    if (executable.isEmpty()) {
        return {false, false, QStringLiteral("S2VNA.exe не найден. Установите S2VNA на этом ПК")};
    }
    const QFileInfo file(executable);
    std::wstring application = QDir::toNativeSeparators(file.absoluteFilePath()).toStdWString();
    std::wstring workDir = QDir::toNativeSeparators(file.absolutePath()).toStdWString();
    std::wstring command = L"\"" + application + L"\" /SocketServer:on /SocketPort:"
        + std::to_wstring(socketPort) + L" /visible:off /simulate:off";

    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                         &limits, sizeof(limits))) {
        if (job) {
            CloseHandle(job);
        }
        return {false, false, QStringLiteral("Не удалось создать защищённый процесс S2VNA")};
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, nullptr, workDir.c_str(),
                        &startup, &process)) {
        const DWORD error = GetLastError();
        CloseHandle(job);
        return {false, false, QStringLiteral("Не удалось запустить S2VNA (Windows %1)").arg(error)};
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        const DWORD error = GetLastError();
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(job);
        return {false, false,
                QStringLiteral("Не удалось привязать S2VNA к AFAR (Windows %1)").arg(error)};
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        const DWORD error = GetLastError();
        CloseHandle(job);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return {false, false, QStringLiteral("Не удалось продолжить запуск S2VNA (Windows %1)").arg(error)};
    }
    CloseHandle(process.hThread);
    m_process = process.hProcess;
    m_job = job;
    m_processId = process.dwProcessId;
    m_socketPort = socketPort;
    return {true, true, QStringLiteral("S2VNA запускается скрыто с Socket Server на порту %1")
                            .arg(socketPort)};
#else
    Q_UNUSED(socketPort)
    return {false, false, QStringLiteral("Автозапуск S2VNA доступен только в Windows")};
#endif
}
