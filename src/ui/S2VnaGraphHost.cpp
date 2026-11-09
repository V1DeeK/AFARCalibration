#include "S2VnaGraphHost.h"

#include <QLabel>
#include <QPixmap>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

#ifdef Q_OS_WIN
struct ShotSearch {
    HWND best = nullptr;
    long long area = 0;
};

BOOL CALLBACK findS2VnaTop(HWND window, LPARAM data)
{
    if (!IsWindowVisible(window)) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return TRUE;
    }
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &length) != 0;
    CloseHandle(process);
    if (!ok) {
        return TRUE;
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    if (_wcsicmp(name != nullptr ? name + 1 : path, L"S2VNA.exe") != 0) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(window, &rect)) {
        return TRUE;
    }
    const long long area = static_cast<long long>(rect.right - rect.left) * (rect.bottom - rect.top);
    if (area < 200 * 150) {
        return TRUE;
    }
    auto* search = reinterpret_cast<ShotSearch*>(data);
    if (area > search->area) {
        search->area = area;
        search->best = window;
    }
    return TRUE;
}

QImage grabWindow(HWND window)
{
    RECT rect{};
    if (window == nullptr || !IsWindow(window) || !GetWindowRect(window, &rect)) {
        return {};
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < 50 || height < 50) {
        return {};
    }
    HDC windowDc = GetWindowDC(window);
    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, width, height);
    const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    const BOOL printed = PrintWindow(window, memoryDc, 0x00000002);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    QImage image(width, height, QImage::Format_ARGB32);
    GetDIBits(memoryDc, bitmap, 0, static_cast<UINT>(height), image.bits(), &info, DIB_RGB_COLORS);
    SelectObject(memoryDc, previous);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(window, windowDc);
    if (printed == FALSE || image.isNull()) {
        return {};
    }
    return image;
}
#endif

}  // namespace

S2VnaGraphHost::S2VnaGraphHost(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("s2vnaGraphHost"));
    setMinimumSize(640, 360);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    m_image = new QLabel(this);
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setScaledContents(true);
    m_image->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    lay->addWidget(m_image);
    m_watch = new QTimer(this);
    m_watch->setInterval(400);
    connect(m_watch, &QTimer::timeout, this, &S2VnaGraphHost::refreshMirror);
}

S2VnaGraphHost::~S2VnaGraphHost()
{
    detach();
}

bool S2VnaGraphHost::attach(QString* error)
{
#ifdef Q_OS_WIN
    if (isAttached()) {
        return true;
    }
    ShotSearch search;
    EnumWindows(findS2VnaTop, reinterpret_cast<LPARAM>(&search));
    if (search.best == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("Окно S2VNA не найдено. Запустите анализатор и откройте C2220.");
        }
        return false;
    }
    m_source = search.best;
    refreshMirror();
    if (m_image->pixmap().isNull()) {
        m_source = nullptr;
        if (error != nullptr) {
            *error = QStringLiteral("Окно S2VNA найдено, но снимок не прочитался.");
        }
        return false;
    }
    m_watch->start();
    return true;
#else
    if (error != nullptr) {
        *error = QStringLiteral("Трансляция окна S2VNA доступна только в Windows.");
    }
    return false;
#endif
}

void S2VnaGraphHost::detach()
{
    m_watch->stop();
#ifdef Q_OS_WIN
    m_source = nullptr;
#endif
    if (m_image != nullptr) {
        m_image->clear();
    }
}

bool S2VnaGraphHost::isAttached() const
{
#ifdef Q_OS_WIN
    const auto window = static_cast<HWND>(m_source);
    return m_watch->isActive() && window != nullptr && IsWindow(window);
#else
    return false;
#endif
}

void S2VnaGraphHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}

void S2VnaGraphHost::refreshMirror()
{
#ifdef Q_OS_WIN
    const auto window = static_cast<HWND>(m_source);
    if (window == nullptr || !IsWindow(window)) {
        detach();
        emit windowLost();
        return;
    }
    const QImage image = grabWindow(window);
    if (image.isNull()) {
        return;
    }
    m_image->setPixmap(QPixmap::fromImage(image));
#else
    Q_UNUSED(this);
#endif
}
