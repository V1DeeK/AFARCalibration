#include "S2VnaGraphHost.h"

#include <QResizeEvent>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>

#include <string>
#include <vector>

namespace {

bool hasClass(HWND window, const wchar_t* expected)
{
    wchar_t name[128]{};
    return GetClassNameW(window, name, 128) > 0 && wcscmp(name, expected) == 0;
}

bool isS2VnaProcess(DWORD pid)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != 0;
    CloseHandle(process);
    if (!ok) return false;
    const wchar_t* name = wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, L"S2VNA.exe") == 0;
}

struct Candidate {
    HWND view = nullptr;
    long long area = 0;
};

struct Search {
    int instances = 0;
    Candidate best;
};

BOOL CALLBACK findView(HWND window, LPARAM data)
{
    if (!hasClass(window, L"TView")) return TRUE;
    // IsWindowVisible возвращает false, когда AFAR скрыла главное окно S2VNA.
    if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE)) return TRUE;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return TRUE;
    const long long width = rect.right - rect.left;
    const long long height = rect.bottom - rect.top;
    const long long area = width * height;
    if (width < 300 || height < 200) return TRUE;
    auto* candidate = reinterpret_cast<Candidate*>(data);
    if (area > candidate->area) *candidate = {window, area};
    return TRUE;
}

BOOL CALLBACK findMain(HWND window, LPARAM data)
{
    if (!hasClass(window, L"TMain")) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!isS2VnaProcess(pid)) return TRUE;
    auto* search = reinterpret_cast<Search*>(data);
    ++search->instances;
    Candidate candidate;
    EnumChildWindows(window, findView, reinterpret_cast<LPARAM>(&candidate));
    if (candidate.area > search->best.area) search->best = candidate;
    return TRUE;
}

} // namespace
#endif

S2VnaGraphHost::S2VnaGraphHost(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("s2vnaGraphHost"));
    setAttribute(Qt::WA_NativeWindow);
    m_watch = new QTimer(this);
    m_watch->setInterval(1000);
    connect(m_watch, &QTimer::timeout, this, [this] {
        if (!isAttached()) {
            detach();
            emit windowLost();
        }
    });
}

S2VnaGraphHost::~S2VnaGraphHost()
{
    detach();
}

bool S2VnaGraphHost::attach(QString* error)
{
#ifdef Q_OS_WIN
    if (isAttached()) return true;
    Search search;
    EnumWindows(findMain, reinterpret_cast<LPARAM>(&search));
    if (search.instances != 1 || !search.best.view) {
        if (error) {
            *error = search.instances > 1
                ? QStringLiteral("Найдено несколько окон S2VNA. Оставьте одно запущенное окно.")
                : QStringLiteral("Область графика S2VNA не найдена. Подключите C2220 через S2VNA.");
        }
        return false;
    }

    const HWND view = search.best.view;
    const HWND original = GetParent(view);
    RECT rect{};
    if (!original || !GetWindowRect(view, &rect)) {
        if (error) *error = QStringLiteral("Не удалось прочитать положение графика S2VNA.");
        return false;
    }
    MapWindowPoints(nullptr, original, reinterpret_cast<POINT*>(&rect), 2);
    const HWND host = reinterpret_cast<HWND>(winId());
    if (!IsWindow(view) || !IsWindow(original) || !IsWindow(host)) {
        if (error) *error = QStringLiteral("Окно графика S2VNA изменилось во время подключения.");
        return false;
    }
    SetLastError(0);
    const HWND previous = SetParent(view, host);
    const DWORD attachError = GetLastError();
    if (previous != original || GetParent(view) != host) {
        if (GetParent(view) == host) SetParent(view, original);
        if (error) *error = QStringLiteral("Windows не разрешила встроить график S2VNA "
                                          "(код %1, старое окно %2, текущее %3).")
                                .arg(attachError)
                                .arg(reinterpret_cast<quintptr>(previous), 0, 16)
                                .arg(reinterpret_cast<quintptr>(GetParent(view)), 0, 16);
        return false;
    }

    m_view = view;
    m_originalParent = original;
    m_host = host;
    m_originalX = rect.left;
    m_originalY = rect.top;
    m_originalWidth = rect.right - rect.left;
    m_originalHeight = rect.bottom - rect.top;
    if (!SetWindowPos(view, nullptr, 0, 0, width(), height(),
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW)) {
        const DWORD moveError = GetLastError();
        detach();
        if (error) *error = QStringLiteral("Не удалось показать график S2VNA (код %1).")
                                .arg(moveError);
        return false;
    }
    m_watch->start();
    return true;
#else
    if (error) *error = QStringLiteral("Встраивание окна S2VNA доступно только в Windows.");
    return false;
#endif
}

void S2VnaGraphHost::detach()
{
    m_watch->stop();
#ifdef Q_OS_WIN
    const HWND view = static_cast<HWND>(m_view);
    const HWND original = static_cast<HWND>(m_originalParent);
    const HWND host = static_cast<HWND>(m_host);
    if (view && IsWindow(view) && original && IsWindow(original) && GetParent(view) == host) {
        SetParent(view, original);
        SetWindowPos(view, nullptr, m_originalX, m_originalY,
                     m_originalWidth, m_originalHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    m_view = nullptr;
    m_originalParent = nullptr;
    m_host = nullptr;
#endif
}

bool S2VnaGraphHost::isAttached() const
{
#ifdef Q_OS_WIN
    const HWND view = static_cast<HWND>(m_view);
    return view && IsWindow(view) && GetParent(view) == static_cast<HWND>(m_host);
#else
    return false;
#endif
}

void S2VnaGraphHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
#ifdef Q_OS_WIN
    if (isAttached()) {
        SetWindowPos(static_cast<HWND>(m_view), nullptr, 0, 0, width(), height(),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}
