#include "FourChannelPanel.h"

#include "Ppkh4Frame.h"
#include "Ppkh4Link.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QImage>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QRandomGenerator>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <vector>

#include <algorithm>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using namespace afar::ppkh4;

constexpr int kLogLines = 200;
constexpr int kSendDebounceMs = 400;
constexpr int kSendGapMs = 500;
constexpr int kSettleMs = 8000;
constexpr int kCollectMs = 2000;
constexpr int kMarkerWaitMs = 20000;
constexpr int kChannels = 4;

#ifdef Q_OS_WIN
struct S2VnaShotSearch {
    HWND best = nullptr;
    long long area = 0;
};

BOOL CALLBACK findS2VnaWindow(HWND window, LPARAM param)
{
    auto* search = reinterpret_cast<S2VnaShotSearch*>(param);
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
    if (area > search->area) {
        search->area = area;
        search->best = window;
    }
    return TRUE;
}

QImage captureS2VnaWindow()
{
    S2VnaShotSearch search;
    EnumWindows(findS2VnaWindow, reinterpret_cast<LPARAM>(&search));
    if (search.best == nullptr) {
        return {};
    }
    RECT rect{};
    if (!GetWindowRect(search.best, &rect)) {
        return {};
    }
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width < 50 || height < 50) {
        return {};
    }
    HDC windowDc = GetWindowDC(search.best);
    HDC memoryDc = CreateCompatibleDC(windowDc);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDc, width, height);
    const HGDIOBJ previous = SelectObject(memoryDc, bitmap);
    const BOOL printed = PrintWindow(search.best, memoryDc, 0x00000002);
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
    ReleaseDC(search.best, windowDc);
    if (printed == FALSE || image.isNull()) {
        return {};
    }
    return image;
}
#else
QImage captureS2VnaWindow()
{
    return {};
}
#endif

QString key(const char* name)
{
    return QStringLiteral("fourChannel/") + QLatin1String(name);
}

QString channelKey(const char* field, int index)
{
    return QStringLiteral("fourChannel/%1/%2").arg(QLatin1String(field)).arg(index);
}

void defaultFormat(FrameFormat& format, LineConfig& lines)
{
    format = FrameFormat{};
    lines = LineConfig{};
}

QString codeText(int code, const FieldFormat& field, bool phase)
{
    const double value = phase ? phase_deg(code, field) : atten_db(code, field);
    const int digits = phase ? 3 : 2;
    const QString unit = phase ? QStringLiteral("°") : QStringLiteral(" дБ");
    return QString::number(value, 'f', digits) + unit;
}

constexpr const char* kChannelColor[4] = {"#1d4ed8", "#15803d", "#c2410c", "#be123c"};
constexpr const char* kPhaseColor = "#6d28d9";
constexpr const char* kAttenColor = "#b45309";
constexpr int kWordBits = 12;

QString bitCell(bool one, const QString& color)
{
    return QStringLiteral("<td align=\"center\" style=\"color:%1;padding:1px 2px\">%2</td>")
        .arg(color, one ? QStringLiteral("<b>1</b>") : QStringLiteral("0"));
}

QString packetHtml(const std::vector<bool>& bits)
{
    const int count = static_cast<int>(bits.size()) / kWordBits;
    QString header;
    QString values;
    QString numbers;
    for (int ch = count - 1; ch >= 0; --ch) {
        const QString color = QLatin1String(kChannelColor[ch]);
        header += QStringLiteral(
                      "<td colspan=\"%1\" align=\"center\" style=\"color:%2\"><b>Канал %3</b></td>")
                      .arg(kWordBits)
                      .arg(color)
                      .arg(ch + 1);
        for (int i = kWordBits - 1; i >= 0; --i) {
            const bool one = bits[static_cast<std::size_t>(ch * kWordBits + i)];
            values += bitCell(one, color);
            numbers += QStringLiteral("<td align=\"center\"><small>%1</small></td>").arg(i);
        }
        if (ch > 0) {
            header += QStringLiteral("<td>&nbsp;</td>");
            values += QStringLiteral("<td></td>");
            numbers += QStringLiteral("<td></td>");
        }
    }
    return QStringLiteral(
               "<table cellspacing=\"0\" cellpadding=\"1\" style=\"font-family:monospace\">"
               "<tr>%1</tr><tr>%2</tr><tr>%3</tr></table>")
        .arg(header, values, numbers);
}

QString channelBitsHtml(const std::vector<bool>& bits, int channel, bool flip, const FrameFormat& format)
{
    const int base = channel * kWordBits;
    QString top;
    QString bottom;
    for (int i = kWordBits - 1; i >= 0; --i) {
        const int src = flip ? (kWordBits - 1 - i) : i;
        QString color = QStringLiteral("#64748b");
        if (src >= format.phase.offset && src < format.phase.offset + format.phase.width) {
            color = QLatin1String(kPhaseColor);
        } else if (src >= format.atten.offset && src < format.atten.offset + format.atten.width) {
            color = QLatin1String(kAttenColor);
        }
        const bool one = base + i < static_cast<int>(bits.size())
                         && bits[static_cast<std::size_t>(base + i)];
        top += bitCell(one, color);
        bottom += QStringLiteral("<td align=\"center\"><small>%1</small></td>").arg(i);
    }
    return QStringLiteral(
               "<table cellspacing=\"2\" cellpadding=\"2\" style=\"font-family:monospace\">"
               "<tr>%1</tr><tr>%2</tr></table>")
        .arg(top, bottom);
}

}  // namespace

using namespace afar::ppkh4;

FourChannelPanel::FourChannelPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("fourChannelPanel"));
    m_link = new Ppkh4Link(this);
    m_sendDebounce = new QTimer(this);
    m_sendDebounce->setSingleShot(true);
    m_sendDebounce->setInterval(kSendDebounceMs);
    m_stepTimer = new QTimer(this);
    m_stepTimer->setSingleShot(true);
    m_stepTimer->setInterval(kSettleMs);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(ui::kGap);
    root->addWidget(makeConnectionBox());
    root->addWidget(makeSelectBox());
    auto* row = new QHBoxLayout();
    row->setSpacing(ui::kGap);
    row->addWidget(makePhaseBox(), 1);
    row->addWidget(makeAttenBox(), 1);
    root->addLayout(row);
    root->addWidget(makeMeasureBox());
    root->addWidget(makeLogBox());

    connect(m_sendDebounce, &QTimer::timeout, this, &FourChannelPanel::requestSend);
    connect(m_stepTimer, &QTimer::timeout, this, &FourChannelPanel::onStepTimer);
    connect(m_link, &Ppkh4Link::opened, this, &FourChannelPanel::onOpened);
    connect(m_link, &Ppkh4Link::closed, this, &FourChannelPanel::onClosed);
    connect(m_link, &Ppkh4Link::commandSent, this,
            [this](const QString& cmd) { appendLog(QStringLiteral("→ ") + cmd); });
    connect(m_link, &Ppkh4Link::lineReceived, this,
            [this](const QString& line) { appendLog(QStringLiteral("← ") + line); });
    connect(m_link, &Ppkh4Link::boardError, this, [this](const QString& text) {
        setConnState(QStringLiteral("ошибка платы: ") + text, false, true);
    });

    loadFormat();
    loadSettings();
    refreshPorts();
    showSelected(false);
    updateButtons();
}

int FourChannelPanel::slotIndex() const
{
    return m_slot->currentIndex();
}

int FourChannelPanel::globalChannel() const
{
    return selectedIndex() + 1;
}

int FourChannelPanel::selectedIndex() const
{
    return m_slot->currentIndex();
}

QWidget* FourChannelPanel::makeConnectionBox()
{
    auto* box = new QGroupBox(QStringLiteral("Плата ППКХ4 · канал X2"), this);
    auto* lay = new QHBoxLayout(box);
    ui::applyGroupLayout(lay);

    m_port = new QComboBox(box);
    m_port->setObjectName(QStringLiteral("fourChannelPort"));
    m_port->setEditable(true);
    m_port->setMinimumContentsLength(14);
    m_port->setToolTip(QStringLiteral("Виртуальный COM-порт платы (USB, VID 0483 / PID 5740)"));
    m_refresh = new QPushButton(QStringLiteral("Обновить"), box);
    ui::setButtonRole(m_refresh, ui::ButtonRole::Secondary);
    m_connect = new QPushButton(QStringLiteral("Подключить"), box);
    m_connect->setObjectName(QStringLiteral("fourChannelConnect"));
    ui::setButtonRole(m_connect, ui::ButtonRole::Primary);
    m_conn = new QLabel(box);
    m_conn->setProperty("role", QStringLiteral("status"));
    m_lastSent = ui::hintLabel(QString(), box);
    m_lastSent->setObjectName(QStringLiteral("fourChannelLastSent"));

    lay->addWidget(ui::captionLabel(QStringLiteral("COM-порт"), box));
    lay->addWidget(m_port);
    lay->addWidget(m_refresh);
    lay->addWidget(m_connect);
    lay->addWidget(m_conn);
    lay->addWidget(m_lastSent, 1);

    connect(m_refresh, &QPushButton::clicked, this, &FourChannelPanel::refreshPorts);
    connect(m_connect, &QPushButton::clicked, this, &FourChannelPanel::toggleConnection);
    setConnState(Ppkh4Link::serialSupported()
                     ? QStringLiteral("не подключено")
                     : QStringLiteral("нет модуля Qt Serial Port"),
                 false, !Ppkh4Link::serialSupported());
    return box;
}

QWidget* FourChannelPanel::makeSelectBox()
{
    auto* box = new QGroupBox(QStringLiteral("Четыре канала одной цепочки"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);

    auto* pick = new QHBoxLayout();
    m_slot = new QComboBox(box);
    m_slot->setObjectName(QStringLiteral("fourChannelSlot"));
    for (int i = 1; i <= kChannels; ++i) {
        m_slot->addItem(QStringLiteral("Канал %1").arg(i));
    }
    m_global = new QLabel(box);
    m_global->setObjectName(QStringLiteral("fourChannelGlobal"));
    m_apply = new QPushButton(QStringLiteral("Выставить цепочку"), box);
    m_apply->setObjectName(QStringLiteral("fourChannelApply"));
    ui::setButtonRole(m_apply, ui::ButtonRole::Primary);
    m_readBoard = new QPushButton(QStringLiteral("Считать с платы"), box);
    m_readBoard->setObjectName(QStringLiteral("fourChannelRead"));
    ui::setButtonRole(m_readBoard, ui::ButtonRole::Secondary);
    pick->addWidget(ui::captionLabel(QStringLiteral("Канал"), box));
    pick->addWidget(m_slot);
    pick->addWidget(m_global, 1);
    pick->addWidget(m_readBoard);
    pick->addWidget(m_apply);
    lay->addLayout(pick);
    m_boardLevel = ui::hintLabel(QStringLiteral("Уровень платы ещё не считан."), box);
    m_boardLevel->setObjectName(QStringLiteral("fourChannelBoardLevel"));
    lay->addWidget(m_boardLevel);

    lay->addWidget(ui::hintLabel(
        QStringLiteral("Каналы стоят в одном SPI. Любая правка сдвигает все четыре слова подряд. "
                       "Канал 1 — ближайший к плате: младшие 12 бит, сдвигаются последними. "
                       "Канал 4 — дальний конец: старшие 12 бит, уходят первыми. "
                       "Если первый физический канал стоит у разъёма платы, выбирайте канал 1. "
                       "В каждом слове биты 11…6 — аттенюация, 5…0 — фаза."),
        box));

    m_packet = new QLabel(box);
    m_packet->setObjectName(QStringLiteral("fourChannelPacket"));
    m_packet->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_packet->setStyleSheet(QStringLiteral("font-family: monospace;"));
    m_packetBits = new QLabel(box);
    m_packetBits->setObjectName(QStringLiteral("fourChannelPacketBits"));
    m_packetBits->setTextFormat(Qt::RichText);
    m_packetBits->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_packet);
    lay->addWidget(m_packetBits);
    lay->addWidget(ui::hintLabel(
        QStringLiteral("Общий пакет, 48 бит. Цвет — канал. Слева канал 4 (уходит первым), "
                       "справа канал 1 (остаётся у платы). Под битом — его номер в слове канала."),
        box));

    for (int i = 0; i < kChannels; ++i) {
        auto* row = new QPushButton(box);
        row->setObjectName(QStringLiteral("fourChannelRow%1").arg(i));
        row->setCheckable(true);
        row->setAutoExclusive(true);
        ui::setButtonRole(row, ui::ButtonRole::Secondary);
        m_rows[static_cast<std::size_t>(i)] = row;
        lay->addWidget(row);
        connect(row, &QPushButton::clicked, this, [this, i]() {
            if (m_running || m_slot->currentIndex() == i) {
                return;
                }
                m_slot->setCurrentIndex(i);
        });

        auto* toggle = new QToolButton(box);
        toggle->setObjectName(QStringLiteral("fourChannelBitsToggle%1").arg(i));
        toggle->setText(QStringLiteral("Биты канала %1").arg(i + 1));
        toggle->setCheckable(true);
        toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        toggle->setArrowType(Qt::RightArrow);
        toggle->setAutoRaise(true);
        m_bitToggles[static_cast<std::size_t>(i)] = toggle;

        auto* panel = new QWidget(box);
        panel->setVisible(false);
        auto* panelLay = new QVBoxLayout(panel);
        panelLay->setContentsMargins(16, 0, 0, 4);
        auto* order = new QComboBox(panel);
        order->setObjectName(QStringLiteral("fourChannelOrder%1").arg(i));
        order->addItem(QStringLiteral("старшим битом вперёд"));
        order->addItem(QStringLiteral("младшим битом вперёд"));
        auto* bits = new QLabel(panel);
        bits->setObjectName(QStringLiteral("fourChannelBits%1").arg(i));
        bits->setTextFormat(Qt::RichText);
        panelLay->addWidget(order);
        panelLay->addWidget(ui::hintLabel(
            QStringLiteral("Фиолетовый — фаза, коричневый — аттенюация. "
                           "Слева бит, который у этого канала уходит раньше. "
                           "Порядок переворачивает только эти 12 бит."),
            panel));
        panelLay->addWidget(bits);
        m_order[static_cast<std::size_t>(i)] = order;
        m_bitViews[static_cast<std::size_t>(i)] = bits;
        m_bitPanels[static_cast<std::size_t>(i)] = panel;
        lay->addWidget(toggle);
        lay->addWidget(panel);
        connect(toggle, &QToolButton::toggled, this, [toggle, panel](bool on) {
            toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            panel->setVisible(on);
        });
        connect(order, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, i](int index) { setChannelOrder(i, index); });
    }

    connect(m_slot, &QComboBox::currentIndexChanged, this, &FourChannelPanel::onSelectionChanged);
    connect(m_readBoard, &QPushButton::clicked, this, &FourChannelPanel::readBoard);
    connect(m_apply, &QPushButton::clicked, this, [this] {
        if (!m_connected || m_running) {
            return;
        }
        applyCfg();
        queueSend(true);
    });
    return box;
}

QWidget* FourChannelPanel::makePhaseBox()
{
    auto* box = new QGroupBox(QStringLiteral("Фаза выбранного канала"), this);
    m_phaseBox = box;
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);
    m_phVal = new QLabel(box);
    m_phVal->setObjectName(QStringLiteral("fourChannelPhaseValue"));
    {
        QFont font = m_phVal->font();
        font.setPointSize(18);
        font.setBold(true);
        m_phVal->setFont(font);
    }
    lay->addWidget(m_phVal);
    m_phRange = new QSlider(Qt::Horizontal, box);
    m_phRange->setObjectName(QStringLiteral("fourChannelPhaseSlider"));
    lay->addWidget(m_phRange);
    auto* controls = new QHBoxLayout();
    auto* down = new QPushButton(QStringLiteral("−1 шаг"), box);
    auto* up = new QPushButton(QStringLiteral("+1 шаг"), box);
    m_phDeg = new QDoubleSpinBox(box);
    m_phDeg->setObjectName(QStringLiteral("fourChannelPhaseDeg"));
    m_phDeg->setDecimals(3);
    m_phDeg->setRange(0.0, 360000.0);
    m_phDeg->setSuffix(QStringLiteral("°"));
    m_phNum = new QSpinBox(box);
    m_phNum->setObjectName(QStringLiteral("fourChannelPhaseCode"));
    controls->addWidget(down);
    controls->addWidget(up);
    controls->addWidget(m_phDeg);
    controls->addWidget(m_phNum);
    lay->addLayout(controls);
    connect(m_phRange, &QSlider::valueChanged, this, &FourChannelPanel::setPhaseCode);
    connect(m_phRange, &QSlider::sliderReleased, this, [this] { queueSend(true); });
    connect(m_phNum, &QSpinBox::valueChanged, this, &FourChannelPanel::setPhaseCode);
    connect(m_phDeg, &QDoubleSpinBox::editingFinished, this,
            [this]() { setPhaseCode(phase_code_from_deg(m_phDeg->value(), m_format.phase)); });
    connect(up, &QPushButton::clicked, this, [this]() { setPhaseCode(m_phCode + 1); });
    connect(down, &QPushButton::clicked, this, [this]() { setPhaseCode(m_phCode - 1); });
    return box;
}

QWidget* FourChannelPanel::makeAttenBox()
{
    auto* box = new QGroupBox(QStringLiteral("Аттенюация выбранного канала"), this);
    m_attenBox = box;
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);
    m_atVal = new QLabel(box);
    m_atVal->setObjectName(QStringLiteral("fourChannelAttenValue"));
    {
        QFont font = m_atVal->font();
        font.setPointSize(18);
        font.setBold(true);
        m_atVal->setFont(font);
    }
    lay->addWidget(m_atVal);
    m_atRange = new QSlider(Qt::Horizontal, box);
    m_atRange->setObjectName(QStringLiteral("fourChannelAttenSlider"));
    lay->addWidget(m_atRange);
    auto* controls = new QHBoxLayout();
    auto* down = new QPushButton(QStringLiteral("−1 шаг"), box);
    auto* up = new QPushButton(QStringLiteral("+1 шаг"), box);
    m_atDb = new QDoubleSpinBox(box);
    m_atDb->setObjectName(QStringLiteral("fourChannelAttenDb"));
    m_atDb->setDecimals(2);
    m_atDb->setSuffix(QStringLiteral(" дБ"));
    m_atNum = new QSpinBox(box);
    m_atNum->setObjectName(QStringLiteral("fourChannelAttenCode"));
    controls->addWidget(down);
    controls->addWidget(up);
    controls->addWidget(m_atDb);
    controls->addWidget(m_atNum);
    lay->addLayout(controls);
    connect(m_atRange, &QSlider::valueChanged, this, &FourChannelPanel::setAttenCode);
    connect(m_atRange, &QSlider::sliderReleased, this, [this] { queueSend(true); });
    connect(m_atNum, &QSpinBox::valueChanged, this, &FourChannelPanel::setAttenCode);
    connect(m_atDb, &QDoubleSpinBox::editingFinished, this,
            [this]() { setAttenCode(atten_code_from_db(m_atDb->value(), m_format.atten)); });
    connect(up, &QPushButton::clicked, this, [this]() { setAttenCode(m_atCode + 1); });
    connect(down, &QPushButton::clicked, this, [this]() { setAttenCode(m_atCode - 1); });
    return box;
}

QWidget* FourChannelPanel::makeLogBox()
{
    auto* box = new QGroupBox(QStringLiteral("Обмен с платой"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);
    m_log = new QPlainTextEdit(box);
    m_log->setObjectName(QStringLiteral("fourChannelLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setFixedHeight(90);
    lay->addWidget(m_log);
    return box;
}

void FourChannelPanel::refreshPorts()
{
    const QString current = m_port->currentText();
    QSignalBlocker block(m_port);
    m_port->clear();
    for (const auto& p : Ppkh4Link::availablePorts()) {
        QString label = p.name;
        if (p.board) {
            label += QStringLiteral(" — ППКХ4");
        } else if (!p.description.isEmpty()) {
            label += QStringLiteral(" — ") + p.description;
        }
        m_port->addItem(label, p.name);
    }
    const int saved = m_port->findData(current.section(QLatin1Char(' '), 0, 0));
    if (saved >= 0) {
        m_port->setCurrentIndex(saved);
    } else if (m_port->count() > 0) {
        m_port->setCurrentIndex(0);
    } else {
        m_port->setEditText(current);
    }
}

void FourChannelPanel::toggleConnection()
{
    if (m_connected) {
        m_link->close();
        onClosed(QString());
        return;
    }
    const QString name = m_port->currentText().section(QLatin1Char(' '), 0, 0);
    if (!m_link->open(name)) {
        setConnState(QStringLiteral("порт не открыт: ") + m_link->lastError(), false, true);
    }
}

void FourChannelPanel::onOpened()
{
    setConnState(QStringLiteral("проверка связи…"), false, false);
    m_link->send(QStringLiteral("PING"), [this](const QString& reply) {
        if (!reply.startsWith(QLatin1String("OK"))) {
            setConnState(QStringLiteral("плата не отвечает"), false, true);
            m_link->close();
            m_connected = false;
            updateButtons();
            return;
        }
        m_connected = true;
        setConnState(QStringLiteral("подключено: ") + m_link->portName(), true, false);
        QSettings().setValue(key("port"), m_link->portName());
        updateButtons();
        loadFormat();
        showSelected(false);
        applyCfg();
        readBoard();
    });
    updateButtons();
}

void FourChannelPanel::onClosed(const QString& reason)
{
    m_sendDebounce->stop();
    m_connected = false;
    if (reason.isEmpty()) {
        setConnState(QStringLiteral("не подключено"), false, false);
    } else {
        setConnState(QStringLiteral("связь потеряна: ") + reason, false, true);
    }
    updateButtons();
}

void FourChannelPanel::setConnState(const QString& text, bool ok, bool bad)
{
    m_conn->setText(text);
    ui::setTone(m_conn, ok ? ui::Tone::Ok : (bad ? ui::Tone::Error : ui::Tone::Neutral));
}

void FourChannelPanel::updateButtons()
{
    const bool open = m_link->isOpen();
    m_connect->setText(open ? QStringLiteral("Отключить") : QStringLiteral("Подключить"));
    ui::setButtonRole(m_connect, open ? ui::ButtonRole::Danger : ui::ButtonRole::Primary);
    m_connect->setEnabled(Ppkh4Link::serialSupported());
    m_port->setEnabled(!open);
    m_refresh->setEnabled(!open);
    m_apply->setEnabled(m_connected);
    if (m_readBoard != nullptr) {
        m_readBoard->setEnabled(m_connected && !m_running);
    }
    setEditEnabled();
}

void FourChannelPanel::setEditEnabled()
{
    const bool edit = m_connected && !m_running;
    const int selected = selectedIndex();
    if (m_phaseBox != nullptr) {
        m_phaseBox->setEnabled(edit);
    }
    if (m_attenBox != nullptr) {
        m_attenBox->setEnabled(edit);
    }
    for (int i = 0; i < kChannels; ++i) {
        const bool thisChannel = edit && i == selected;
        const auto index = static_cast<std::size_t>(i);
        const auto enable = [thisChannel](QWidget* widget) {
            if (widget != nullptr) {
                widget->setEnabled(thisChannel);
            }
        };
        enable(m_order[index]);
        enable(m_sidePhaseSlider[index]);
        enable(m_sidePhase[index]);
        enable(m_sidePhaseDeg[index]);
        enable(m_sideAttenSlider[index]);
        enable(m_sideAtten[index]);
        enable(m_sideAttenDb[index]);
    }
}

void FourChannelPanel::loadFormat()
{
    defaultFormat(m_format, m_lines);
    for (int i = 0; i < kChannels; ++i) {
        m_phase[static_cast<std::size_t>(i)] = clamp_code(m_phase[static_cast<std::size_t>(i)], m_format.phase);
        m_atten[static_cast<std::size_t>(i)] = clamp_code(m_atten[static_cast<std::size_t>(i)], m_format.atten);
    }
}

void FourChannelPanel::loadSettings()
{
    QSettings s;
    m_loading = true;
    for (int i = 0; i < kChannels; ++i) {
        m_phase[static_cast<std::size_t>(i)] =
            clamp_code(s.value(channelKey("phase", i), 0).toInt(), m_format.phase);
        m_atten[static_cast<std::size_t>(i)] =
            clamp_code(s.value(channelKey("atten", i), 0).toInt(), m_format.atten);
        m_lsb[static_cast<std::size_t>(i)] =
            s.value(channelKey("lsb", i), 0).toInt() != 0 ? 1 : 0;
        m_confirmedPhase[static_cast<std::size_t>(i)] = m_phase[static_cast<std::size_t>(i)];
        m_confirmedAtten[static_cast<std::size_t>(i)] = m_atten[static_cast<std::size_t>(i)];
        m_confirmedLsb[static_cast<std::size_t>(i)] = m_lsb[static_cast<std::size_t>(i)];
    }
    const int slot = std::clamp(s.value(key("slot"), 0).toInt(), 0, kChannels - 1);
    m_slot->setCurrentIndex(slot);
    if (m_product != nullptr) {
        m_product->setCurrentIndex(std::clamp(s.value(key("product"), 0).toInt(), 0, 3));
    }
    m_port->setEditText(s.value(key("port")).toString());
    m_loading = false;
}

void FourChannelPanel::saveSelection() const
{
    QSettings s;
    s.setValue(key("slot"), m_slot->currentIndex());
}

void FourChannelPanel::saveChannel(int index) const
{
    QSettings s;
    s.setValue(channelKey("phase", index), m_phase[static_cast<std::size_t>(index)]);
    s.setValue(channelKey("atten", index), m_atten[static_cast<std::size_t>(index)]);
    s.setValue(channelKey("lsb", index), m_lsb[static_cast<std::size_t>(index)]);
    s.setValue(key("slot"), m_slot->currentIndex());
}

void FourChannelPanel::onSelectionChanged()
{
    if (m_loading || m_running) {
        return;
    }
    saveSelection();
    showSelected(false);
    setEditEnabled();
}

void FourChannelPanel::showSelected(bool sendNow)
{
    loadFormat();
    const int index = selectedIndex();
    m_phCode = m_phase[static_cast<std::size_t>(index)];
    m_atCode = m_atten[static_cast<std::size_t>(index)];
    m_global->setText(QStringLiteral("Канал %1 из 4").arg(m_slot->currentIndex() + 1));
    render();
    if (sendNow && m_connected) {
        queueSend(true);
    }
}

void FourChannelPanel::refreshRows()
{
    const int slot = m_slot->currentIndex();
    for (int i = 0; i < kChannels; ++i) {
        const int phase = m_phase[static_cast<std::size_t>(i)];
        const int atten = m_atten[static_cast<std::size_t>(i)];
        auto* row = m_rows[static_cast<std::size_t>(i)];
        QString text;
        if (i == slot) {
            text = QStringLiteral("Канал %1 · фаза %2 · аттенюация %3")
                         .arg(i + 1)
                         .arg(codeText(phase, m_format.phase, true))
                       .arg(codeText(atten, m_format.atten, false));
            if (m_lsb[static_cast<std::size_t>(i)] != 0) {
                text += QStringLiteral(" · младшим вперёд");
            }
        } else {
            text = QStringLiteral("Канал %1 · в кадре закрыт (−31.5 дБ)").arg(i + 1);
        }
        row->setText(text);
        if (m_sideValue[static_cast<std::size_t>(i)] != nullptr) {
            m_sideValue[static_cast<std::size_t>(i)]->setText(text);
        }
        const QSignalBlocker block(row);
        row->setChecked(i == slot);
    }
}

void FourChannelPanel::setPhaseCode(int code)
{
    setChannelPhase(selectedIndex(), code);
}

void FourChannelPanel::setAttenCode(int code)
{
    setChannelAtten(selectedIndex(), code);
}

int FourChannelPanel::phaseCodeAt(int index) const
{
    if (index < 0 || index >= kChannels) {
        return 0;
    }
    return m_phase[static_cast<std::size_t>(index)];
}

int FourChannelPanel::attenCodeAt(int index) const
{
    if (index < 0 || index >= kChannels) {
        return 0;
    }
    return m_atten[static_cast<std::size_t>(index)];
}

void FourChannelPanel::setChannelPhase(int index, int code)
{
    if (m_running || m_loading || index < 0 || index >= kChannels || index != selectedIndex()) {
        return;
    }
    const int next = clamp_code(code, m_format.phase);
    if (m_phase[static_cast<std::size_t>(index)] == next) {
        return;
    }
    m_phase[static_cast<std::size_t>(index)] = next;
    m_phCode = m_phase[static_cast<std::size_t>(selectedIndex())];
    m_atCode = m_atten[static_cast<std::size_t>(selectedIndex())];
    m_global->setText(QStringLiteral("Канал %1 из 4").arg(selectedIndex() + 1));
    saveChannel(index);
    render();
    armSend();
}

void FourChannelPanel::setChannelAtten(int index, int code)
{
    if (m_running || m_loading || index < 0 || index >= kChannels || index != selectedIndex()) {
        return;
    }
    const int next = clamp_code(code, m_format.atten);
    if (m_atten[static_cast<std::size_t>(index)] == next) {
        return;
    }
    m_atten[static_cast<std::size_t>(index)] = next;
    m_phCode = m_phase[static_cast<std::size_t>(selectedIndex())];
    m_atCode = m_atten[static_cast<std::size_t>(selectedIndex())];
    m_global->setText(QStringLiteral("Канал %1 из 4").arg(selectedIndex() + 1));
    saveChannel(index);
    render();
    armSend();
}

void FourChannelPanel::setChannelOrder(int index, int lsbFirst)
{
    if (m_loading || m_running || index < 0 || index >= kChannels || index != selectedIndex()) {
        return;
    }
    const int next = lsbFirst != 0 ? 1 : 0;
    if (m_lsb[static_cast<std::size_t>(index)] == next) {
        return;
    }
    m_lsb[static_cast<std::size_t>(index)] = next;
    saveChannel(index);
    render();
    armSend();
}

void FourChannelPanel::render()
{
    const int phMax = max_code(m_format.phase.width);
    const int atMax = max_code(m_format.atten.width);
    const double deg = phase_deg(m_phCode, m_format.phase);
    const double db = atten_db(m_atCode, m_format.atten);
    const QSignalBlocker blockPhase(m_phRange);
    const QSignalBlocker blockPhaseNum(m_phNum);
    const QSignalBlocker blockAtten(m_atRange);
    const QSignalBlocker blockAttenNum(m_atNum);
    m_phNum->setRange(0, phMax);
    m_atNum->setRange(0, atMax);
    if (!m_phRange->isSliderDown()) {
        m_phRange->setRange(0, phMax);
    m_phRange->setValue(m_phCode);
    }
    if (!m_atRange->isSliderDown()) {
        m_atRange->setRange(0, atMax);
    m_atRange->setValue(m_atCode);
    }
    if (!m_phNum->hasFocus()) {
        m_phNum->setValue(m_phCode);
    }
    if (!m_atNum->hasFocus()) {
        m_atNum->setValue(m_atCode);
    }
    m_phDeg->setRange(0.0, phase_deg(phMax, m_format.phase));
    m_phDeg->setSingleStep(m_format.phase.lsb);
    m_atDb->setRange(atten_db(0, m_format.atten), 0.0);
    m_atDb->setSingleStep(m_format.atten.lsb);
    if (!m_phDeg->hasFocus()) {
        m_phDeg->setValue(deg);
    }
    if (!m_atDb->hasFocus()) {
        m_atDb->setValue(db);
    }
    m_phVal->setText(codeText(m_phCode, m_format.phase, true));
    m_atVal->setText(codeText(m_atCode, m_format.atten, false));
    refreshPacket();
    refreshRows();
}

void FourChannelPanel::outgoingCodes(std::array<int, 4>& phase, std::array<int, 4>& atten) const
{
    phase = m_phase;
    atten = m_atten;
    const int selected = selectedIndex();
    for (int i = 0; i < kChannels; ++i) {
        if (i == selected) {
            continue;
        }
        phase[static_cast<std::size_t>(i)] = 0;
        atten[static_cast<std::size_t>(i)] = 0;
    }
}

void FourChannelPanel::refreshPacket()
{
    std::array<int, 4> wirePhase{};
    std::array<int, 4> wireAtten{};
    outgoingCodes(wirePhase, wireAtten);
    const auto bits = chain_bits(m_format, wirePhase, wireAtten, m_lsb);
    if (m_packet != nullptr) {
        m_packet->setText(QString::fromStdString(
            send_chain_command(m_format, wirePhase, wireAtten, m_lsb)));
    }
    if (m_packetBits != nullptr) {
        m_packetBits->setText(packetHtml(bits));
    }
    const int phMax = max_code(m_format.phase.width);
    const int atMax = max_code(m_format.atten.width);
    for (int i = 0; i < kChannels; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const bool flip = m_lsb[index] != 0;
        if (m_order[index] != nullptr && m_order[index]->currentIndex() != (flip ? 1 : 0)) {
            const QSignalBlocker block(m_order[index]);
            m_order[index]->setCurrentIndex(flip ? 1 : 0);
        }
        if (m_bitViews[index] != nullptr) {
            m_bitViews[index]->setText(channelBitsHtml(bits, i, flip, m_format));
        }
        const int phase = m_phase[index];
        const int atten = m_atten[index];
        if (m_sidePhaseSlider[index] != nullptr && !m_sidePhaseSlider[index]->isSliderDown()) {
            const QSignalBlocker block(m_sidePhaseSlider[index]);
            m_sidePhaseSlider[index]->setRange(0, phMax);
            m_sidePhaseSlider[index]->setValue(phase);
        }
        if (m_sidePhase[index] != nullptr) {
            const QSignalBlocker block(m_sidePhase[index]);
            m_sidePhase[index]->setRange(0, phMax);
            if (!m_sidePhase[index]->hasFocus()) {
                m_sidePhase[index]->setValue(phase);
            }
        }
        if (m_sidePhaseDeg[index] != nullptr) {
            const QSignalBlocker block(m_sidePhaseDeg[index]);
            m_sidePhaseDeg[index]->setRange(0.0, phase_deg(phMax, m_format.phase));
            m_sidePhaseDeg[index]->setSingleStep(m_format.phase.lsb);
            if (!m_sidePhaseDeg[index]->hasFocus()) {
                m_sidePhaseDeg[index]->setValue(phase_deg(phase, m_format.phase));
            }
        }
        if (m_sideAttenSlider[index] != nullptr && !m_sideAttenSlider[index]->isSliderDown()) {
            const QSignalBlocker block(m_sideAttenSlider[index]);
            m_sideAttenSlider[index]->setRange(0, atMax);
            m_sideAttenSlider[index]->setValue(atten);
        }
        if (m_sideAtten[index] != nullptr) {
            const QSignalBlocker block(m_sideAtten[index]);
            m_sideAtten[index]->setRange(0, atMax);
            if (!m_sideAtten[index]->hasFocus()) {
                m_sideAtten[index]->setValue(atten);
            }
        }
        if (m_sideAttenDb[index] != nullptr) {
            const QSignalBlocker block(m_sideAttenDb[index]);
            m_sideAttenDb[index]->setRange(atten_db(0, m_format.atten), 0.0);
            m_sideAttenDb[index]->setSingleStep(m_format.atten.lsb);
            if (!m_sideAttenDb[index]->hasFocus()) {
                m_sideAttenDb[index]->setValue(atten_db(atten, m_format.atten));
            }
        }
    }
}

QWidget* FourChannelPanel::sideControl(QWidget* parent)
{
    if (m_side == nullptr) {
        buildSideControl(parent);
        render();
    } else if (parent != nullptr) {
        m_side->setParent(parent);
    }
    return m_side;
}

void FourChannelPanel::buildSideControl(QWidget* parent)
{
    auto* box = new QGroupBox(QStringLiteral("Каналы ППКХ4"), parent);
    box->setObjectName(QStringLiteral("graphChannelPanel"));
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);
    lay->addWidget(ui::hintLabel(
        QStringLiteral("Те же коды, что на вкладке «Настройка 4 каналов». "
                       "Код 0 аттенюации — −31.5 дБ, код 63 — 0 дБ."),
        box));
    for (int i = 0; i < kChannels; ++i) {
        const auto index = static_cast<std::size_t>(i);
        auto* title = new QLabel(box);
        title->setObjectName(QStringLiteral("graphChannelValue%1").arg(i));
        title->setWordWrap(true);
        m_sideValue[index] = title;
        lay->addWidget(title);

        auto* phaseRow = new QHBoxLayout();
        auto* phaseSlider = new QSlider(Qt::Horizontal, box);
        phaseSlider->setObjectName(QStringLiteral("graphChannelPhaseSlider%1").arg(i));
        auto* phaseDeg = new QDoubleSpinBox(box);
        phaseDeg->setDecimals(3);
        phaseDeg->setSuffix(QStringLiteral("°"));
        auto* phaseCode = new QSpinBox(box);
        phaseCode->setObjectName(QStringLiteral("graphChannelPhase%1").arg(i));
        phaseCode->setPrefix(QStringLiteral("код "));
        m_sidePhaseSlider[index] = phaseSlider;
        m_sidePhaseDeg[index] = phaseDeg;
        m_sidePhase[index] = phaseCode;
        phaseRow->addWidget(phaseSlider, 1);
        phaseRow->addWidget(phaseDeg);
        phaseRow->addWidget(phaseCode);
        lay->addLayout(phaseRow);

        auto* attenRow = new QHBoxLayout();
        auto* attenSlider = new QSlider(Qt::Horizontal, box);
        attenSlider->setObjectName(QStringLiteral("graphChannelAttenSlider%1").arg(i));
        auto* attenDb = new QDoubleSpinBox(box);
        attenDb->setDecimals(2);
        attenDb->setSuffix(QStringLiteral(" дБ"));
        auto* attenCode = new QSpinBox(box);
        attenCode->setObjectName(QStringLiteral("graphChannelAtten%1").arg(i));
        attenCode->setPrefix(QStringLiteral("код "));
        m_sideAttenSlider[index] = attenSlider;
        m_sideAttenDb[index] = attenDb;
        m_sideAtten[index] = attenCode;
        attenRow->addWidget(attenSlider, 1);
        attenRow->addWidget(attenDb);
        attenRow->addWidget(attenCode);
        lay->addLayout(attenRow);

        connect(phaseSlider, &QSlider::valueChanged, this,
                [this, i](int value) { setChannelPhase(i, value); });
        connect(phaseSlider, &QSlider::sliderReleased, this, [this] { queueSend(true); });
        connect(phaseCode, &QSpinBox::valueChanged, this,
                [this, i](int value) { setChannelPhase(i, value); });
        connect(phaseDeg, &QDoubleSpinBox::editingFinished, this, [this, i, phaseDeg]() {
            setChannelPhase(i, phase_code_from_deg(phaseDeg->value(), m_format.phase));
        });
        connect(attenSlider, &QSlider::valueChanged, this,
                [this, i](int value) { setChannelAtten(i, value); });
        connect(attenSlider, &QSlider::sliderReleased, this, [this] { queueSend(true); });
        connect(attenCode, &QSpinBox::valueChanged, this,
                [this, i](int value) { setChannelAtten(i, value); });
        connect(attenDb, &QDoubleSpinBox::editingFinished, this, [this, i, attenDb]() {
            setChannelAtten(i, atten_code_from_db(attenDb->value(), m_format.atten));
        });
    }
    m_side = box;
    setEditEnabled();
}

void FourChannelPanel::readBoard()
{
    if (!m_connected || m_running) {
        return;
    }
    m_link->send(QStringLiteral("STATUS"), [this](const QString& reply) { applyBoardFrame(reply); });
}

void FourChannelPanel::applyBoardFrame(const QString& reply)
{
    const StatusFrame status = parse_status(reply.toStdString());
    if (m_boardLevel == nullptr) {
        return;
    }
    if (!status.ok) {
        m_boardLevel->setText(reply.startsWith(QLatin1String("ERR"))
                                  ? reply
                                  : QStringLiteral("Плата не ответила на STATUS."));
        ui::setTone(m_boardLevel, ui::Tone::Error);
        return;
    }
    if (!status.has_frame) {
        m_boardLevel->setText(QStringLiteral("Плата ещё не сдвигала кадр. Ползунок показывает только экран."));
        ui::setTone(m_boardLevel, ui::Tone::Warn);
        return;
    }
    std::array<int, 4> phase{};
    std::array<int, 4> atten{};
    if (status.bits != kChannels * m_format.total_bits
        || !decode_chain(m_format, status.hex, phase, atten, m_lsb)) {
        m_boardLevel->setText(QStringLiteral("Кадр платы не разобран: ")
                              + QString::fromStdString(status.hex));
        ui::setTone(m_boardLevel, ui::Tone::Error);
        return;
    }
    const bool canAdopt = !m_userAdjusted && !sliderHeld() && !m_running && !m_sendInFlight;
    if (canAdopt) {
        m_loading = true;
        m_phase = phase;
        m_atten = atten;
        m_confirmedPhase = phase;
        m_confirmedAtten = atten;
        m_confirmedLsb = m_lsb;
        const int selected = selectedIndex();
        m_phCode = m_phase[static_cast<std::size_t>(selected)];
        m_atCode = m_atten[static_cast<std::size_t>(selected)];
        for (int i = 0; i < kChannels; ++i) {
            saveChannel(i);
        }
        render();
        m_loading = false;
    }
    QStringList levels;
    for (int i = 0; i < kChannels; ++i) {
        levels.append(QStringLiteral("к%1 %2, %3")
                          .arg(i + 1)
                          .arg(codeText(phase[static_cast<std::size_t>(i)], m_format.phase, true))
                          .arg(codeText(atten[static_cast<std::size_t>(i)], m_format.atten, false)));
    }
    const QString shown = levels.join(QStringLiteral("; "));
    const bool stuck = m_userAdjusted && !m_lastSentHex.isEmpty()
                       && m_lastSentHex.compare(QString::fromStdString(status.hex), Qt::CaseInsensitive) != 0;
    if (stuck) {
        m_boardLevel->setText(QStringLiteral("Запись ушла, но плата оставила прежний кадр: ") + shown);
        ui::setTone(m_boardLevel, ui::Tone::Error);
        return;
    }
    m_boardLevel->setText(QStringLiteral("На плате, последний сдвинутый кадр: ") + shown);
    ui::setTone(m_boardLevel, ui::Tone::Ok);
    bool othersOpen = false;
    for (int i = 0; i < kChannels; ++i) {
        if (i == selectedIndex()) {
            continue;
        }
        if (phase[static_cast<std::size_t>(i)] != 0 || atten[static_cast<std::size_t>(i)] != 0) {
            othersOpen = true;
        }
    }
    if (othersOpen && !m_userAdjusted && !m_running && !m_sendInFlight && !sliderHeld()) {
        queueSend(true);
    }
}

void FourChannelPanel::applyCfg()
{
    for (const auto& cmd : cfg_commands(m_lines)) {
        m_link->send(QString::fromStdString(cmd));
    }
}

bool FourChannelPanel::sliderHeld() const
{
    const auto down = [](const QSlider* slider) {
        return slider != nullptr && slider->isSliderDown();
    };
    if (down(m_phRange) || down(m_atRange)) {
        return true;
    }
    for (int i = 0; i < kChannels; ++i) {
        const auto index = static_cast<std::size_t>(i);
        if (down(m_sidePhaseSlider[index]) || down(m_sideAttenSlider[index])) {
            return true;
        }
    }
    return false;
}

bool FourChannelPanel::codesDiffer() const
{
    return m_phase != m_confirmedPhase || m_atten != m_confirmedAtten || m_lsb != m_confirmedLsb;
}

void FourChannelPanel::armSend()
{
    if (!m_connected || m_running) {
        return;
    }
    m_userAdjusted = true;
    m_sendForce = true;
    if (sliderHeld()) {
        m_sendDebounce->stop();
        if (m_lastSent != nullptr) {
            m_lastSent->setText(QStringLiteral("Отпустите ползунок — кадр уйдёт на плату."));
            ui::setTone(m_lastSent, ui::Tone::Warn);
        }
        return;
    }
    m_sendDebounce->start(kSendDebounceMs);
}

void FourChannelPanel::queueSend(bool force)
{
    if (force) {
        m_sendForce = true;
    }
    if (sliderHeld()) {
        return;
    }
    requestSend();
}

void FourChannelPanel::requestSend()
{
    if (!m_connected || m_running) {
        return;
    }
    if (sliderHeld()) {
        return;
    }
    if (!m_sendForce && !codesDiffer()) {
        m_sendDirty = false;
        return;
    }
    if (m_sendInFlight) {
        m_sendDirty = true;
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_sendNotBefore > now) {
        m_sendDebounce->start(static_cast<int>(m_sendNotBefore - now));
        return;
    }
    m_sendInFlight = true;
    m_sendDirty = false;
    m_sendForce = false;
    sendFrame([this](bool ok) {
        m_sendInFlight = false;
        m_sendNotBefore = QDateTime::currentMSecsSinceEpoch() + kSendGapMs;
        if (m_running) {
            return;
        }
        if (!ok) {
            m_sendDirty = false;
            m_sendForce = false;
            revertConfirmed(QStringLiteral("плата не подтвердила кадр"));
            setMeasureUi();
            return;
        }
        const bool drifted = commitConfirmed();
        if (drifted && m_connected && !m_running) {
            m_sendForce = true;
            m_sendDebounce->start(kSendGapMs);
        } else {
            m_sendDirty = false;
            m_sendForce = false;
        }
        setMeasureUi();
    });
}

void FourChannelPanel::lockUnselected()
{
    const int selected = selectedIndex();
    for (int i = 0; i < kChannels; ++i) {
        if (i == selected) {
            continue;
        }
        m_phase[static_cast<std::size_t>(i)] = m_confirmedPhase[static_cast<std::size_t>(i)];
        m_atten[static_cast<std::size_t>(i)] = m_confirmedAtten[static_cast<std::size_t>(i)];
    }
}

bool FourChannelPanel::commitConfirmed()
{
    m_confirmedPhase = m_sentPhase;
    m_confirmedAtten = m_sentAtten;
    m_confirmedLsb = m_sentLsb;
    const bool drifted = codesDiffer();
    QStringList others;
    for (int i = 0; i < kChannels; ++i) {
        if (i != selectedIndex()) {
            others.append(QString::number(i + 1));
        }
    }
    if (drifted) {
        m_lastSent->setText(QStringLiteral("Плата подтвердила прежний кадр канала %1. Новый ещё не отправлен.")
                                .arg(selectedIndex() + 1));
        ui::setTone(m_lastSent, ui::Tone::Warn);
    } else {
        m_lastSent->setText(QStringLiteral(
                                "Плата подтвердила канал %1. Каналы %2 в кадре закрыты на −31.5 дБ.")
                                .arg(selectedIndex() + 1)
                                .arg(others.join(QStringLiteral(", "))));
        ui::setTone(m_lastSent, ui::Tone::Ok);
    }
    render();
    return drifted;
}

void FourChannelPanel::revertConfirmed(const QString& reason)
{
    m_phase = m_confirmedPhase;
    m_atten = m_confirmedAtten;
    m_lsb = m_confirmedLsb;
    const int selected = selectedIndex();
    m_phCode = m_phase[static_cast<std::size_t>(selected)];
    m_atCode = m_atten[static_cast<std::size_t>(selected)];
    for (int i = 0; i < kChannels; ++i) {
        saveChannel(i);
    }
    m_lastSent->setText(QStringLiteral("%1. На плате остался последний подтверждённый кадр.").arg(reason));
    ui::setTone(m_lastSent, ui::Tone::Error);
    render();
}

void FourChannelPanel::sendFrame()
{
    sendFrame({});
}

void FourChannelPanel::sendFrame(const std::function<void(bool)>& after)
{
    m_sendDebounce->stop();
    lockUnselected();
    m_sentPhase = m_phase;
    m_sentAtten = m_atten;
    m_sentLsb = m_lsb;
    if (!m_connected) {
        if (after) {
            after(false);
        }
        return;
    }
    std::array<int, 4> wirePhase{};
    std::array<int, 4> wireAtten{};
    outgoingCodes(wirePhase, wireAtten);
    const QString command = QString::fromStdString(
        send_chain_command(m_format, wirePhase, wireAtten, m_lsb));
    const int hexAt = command.lastIndexOf(QLatin1Char(' '));
    m_lastSentHex = hexAt >= 0 ? command.mid(hexAt + 1) : QString();
    const int channel = m_slot->currentIndex() + 1;
    m_link->send(command, [this, command, channel, after](const QString& reply) {
        const bool ok = reply.startsWith(QLatin1String("OK"));
        if (ok) {
            m_lastSent->setText(QStringLiteral("цепочка записана, правится канал %1: %2")
                                    .arg(channel)
                                    .arg(command));
            ui::setTone(m_lastSent, ui::Tone::Ok);
        } else if (reply.startsWith(QLatin1String("ERR"))) {
            m_lastSent->setText(QStringLiteral("канал %1: %2").arg(channel).arg(reply));
            ui::setTone(m_lastSent, ui::Tone::Error);
        }
        if (after) {
            after(ok);
        }
    });
}

QWidget* FourChannelPanel::makeMeasureBox()
{
    auto* box = new QGroupBox(QStringLiteral("Измерение S-параметров"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);
    auto* buttons = new QHBoxLayout();
    m_measureStart = new QPushButton(QStringLiteral("Начать измерение"), box);
    m_measureStart->setObjectName(QStringLiteral("fourChannelMeasureStart"));
    ui::setButtonRole(m_measureStart, ui::ButtonRole::Primary);
    m_measureAll = new QPushButton(QStringLiteral("Все каналы"), box);
    m_measureAll->setObjectName(QStringLiteral("fourChannelMeasureAll"));
    ui::setButtonRole(m_measureAll, ui::ButtonRole::Primary);
    m_repeatCount = new QComboBox(box);
    m_repeatCount->setObjectName(QStringLiteral("fourChannelRepeatCount"));
    m_repeatCount->addItem(QStringLiteral("10 раз"), 10);
    m_repeatCount->addItem(QStringLiteral("20 раз"), 20);
    m_repeatCount->addItem(QStringLiteral("100 раз"), 100);
    m_repeatStart = new QPushButton(QStringLiteral("Повторяемость"), box);
    m_repeatStart->setObjectName(QStringLiteral("fourChannelRepeat"));
    ui::setButtonRole(m_repeatStart, ui::ButtonRole::Success);
    m_measureStop = new QPushButton(QStringLiteral("Остановить"), box);
    m_measureStop->setObjectName(QStringLiteral("fourChannelMeasureStop"));
    ui::setButtonRole(m_measureStop, ui::ButtonRole::Danger);
    m_product = new QComboBox(box);
    m_product->setObjectName(QStringLiteral("fourChannelProduct"));
    for (int i = 1; i <= 4; ++i) {
        m_product->addItem(QStringLiteral("Изделие %1").arg(i));
    }
    connect(m_product, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (m_loading) {
            return;
        }
        QSettings s;
        s.setValue(key("product"), index);
    });
    m_restoreProduct = new QPushButton(QStringLiteral("Восстановить изделие"), box);
    m_restoreProduct->setObjectName(QStringLiteral("fourChannelRestoreProduct"));
    connect(m_restoreProduct, &QPushButton::clicked, this, &FourChannelPanel::restoreProduct);
    buttons->addWidget(m_measureStart);
    buttons->addWidget(m_measureAll);
    buttons->addWidget(m_product);
    buttons->addWidget(m_restoreProduct);
    buttons->addWidget(m_repeatCount);
    buttons->addWidget(m_repeatStart);
    m_openReport = new QPushButton(QStringLiteral("Открыть отчёт"), box);
    m_openReport->setObjectName(QStringLiteral("fourChannelOpenReport"));
    ui::setButtonRole(m_openReport, ui::ButtonRole::Secondary);
    buttons->addWidget(m_measureStop);
    buttons->addWidget(m_openReport);
    lay->addLayout(buttons);
    connect(m_measureStart, &QPushButton::clicked, this, &FourChannelPanel::startSweep);
    connect(m_measureAll, &QPushButton::clicked, this, &FourChannelPanel::startAllChannels);
    connect(m_repeatStart, &QPushButton::clicked, this, &FourChannelPanel::startRepeat);
    connect(m_measureStop, &QPushButton::clicked, this, [this] {
        if (!m_running) {
            return;
        }
        stopMeasure(QStringLiteral("Остановлено."));
    });
    connect(m_openReport, &QPushButton::clicked, this, &FourChannelPanel::openReportFiles);

    m_configPath = ui::hintLabel(productChannelConfigPath(), box);
    m_configPath->setObjectName(QStringLiteral("fourChannelConfigPath"));
    m_configPath->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(m_configPath);
    m_measureStatus = ui::hintLabel(QStringLiteral("Измерение не запущено."), box);
    m_measureStatus->setObjectName(QStringLiteral("fourChannelMeasureStatus"));
    m_measureBar = new QProgressBar(box);
    m_measureBar->setObjectName(QStringLiteral("fourChannelMeasureBar"));
    m_measureBar->setTextVisible(true);
    lay->addWidget(m_measureStatus);
    lay->addWidget(m_measureBar);
    setMeasureUi();
    return box;
}

void FourChannelPanel::startSweep()
{
    beginSweep(false);
}

void FourChannelPanel::startAllChannels()
{
    beginSweep(true);
}

void FourChannelPanel::beginSweep(bool allChannels)
{
    if (m_running || m_sendInFlight) {
        m_measureStatus->setText(m_sendInFlight
                                     ? QStringLiteral("Дождитесь подтверждения платы.")
                                     : QStringLiteral("Измерение уже идёт."));
        ui::setTone(m_measureStatus, ui::Tone::Warn);
        return;
    }
    if (!m_connected) {
        m_measureStatus->setText(QStringLiteral("Сначала подключите плату ППКХ4."));
        ui::setTone(m_measureStatus, ui::Tone::Warn);
        return;
    }
    m_allChannels = allChannels;
    if (allChannels && m_slot != nullptr) {
        const QSignalBlocker block(m_slot);
        m_slot->setCurrentIndex(0);
        saveSelection();
        showSelected(false);
    }
    m_savedPhase = m_confirmedPhase;
    m_savedAtten = m_confirmedAtten;
    m_steps = buildChannelSteps({selectedIndex()}, max_code(m_format.phase.width),
                                max_code(m_format.atten.width), m_savedPhase, m_savedAtten);
    if (m_steps.isEmpty()) {
        m_allChannels = false;
        m_measureStatus->setText(QStringLiteral("Нет шагов для измерения."));
        return;
    }
    chooseScreenshots();
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/channel-reports");
    m_runTitle = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    m_runDir = root + QLatin1Char('/') + m_runTitle;
    QDir().mkpath(m_runDir + QStringLiteral("/graphs"));
    m_points.clear();
    m_pointBase = 0;
    m_stepIndex = 0;
    m_stopAsked = false;
    m_runKind = RunKind::Sweep;
    m_stepPhase = StepPhase::Idle;
    m_running = true;
    m_link->cancelQueued();
    m_measureBar->setRange(0, m_steps.size());
    m_measureBar->setValue(0);
    m_gridPending = true;
    m_sweepNote.clear();
    setMeasureUi();
    m_measureStatus->setText(QStringLiteral("Ставлю одну частоту — середину полосы."));
    appendLog(QStringLiteral("старт измерения: жду анализатор, кадры на плату пойдут после его ответа"));
    emit channelGridRequested();
}

void FourChannelPanel::startRepeat()
{
    if (m_running || m_sendInFlight) {
        m_measureStatus->setText(QStringLiteral("Дождитесь окончания текущего прохода."));
        ui::setTone(m_measureStatus, ui::Tone::Warn);
        return;
    }
    if (!m_connected) {
        m_measureStatus->setText(QStringLiteral("Сначала подключите плату ППКХ4."));
        ui::setTone(m_measureStatus, ui::Tone::Warn);
        return;
    }
    m_repeatTarget = m_repeatCount->currentData().toInt();
    if (m_repeatTarget < 2) {
        m_repeatTarget = 10;
    }
    m_repeatDb.clear();
    m_repeatParameter = -1;
    m_repeatGot = false;
    m_stepIndex = 0;
    m_stopAsked = false;
    m_runKind = RunKind::Repeat;
    m_stepPhase = StepPhase::Idle;
    m_running = true;
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/channel-reports");
    m_runTitle = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
    m_runDir = root + QLatin1Char('/') + m_runTitle + QStringLiteral("-repeat");
    QDir().mkpath(m_runDir);
    m_link->cancelQueued();
    m_measureBar->setRange(0, m_repeatTarget);
    m_measureBar->setValue(0);
    setMeasureUi();
    applyMeasureStep();
}

void FourChannelPanel::stopMeasure(const QString& outcome)
{
    if (!m_running) {
        return;
    }
    const RunKind kind = m_runKind;
    m_stopAsked = true;
    m_gridPending = false;
    m_allChannels = false;
    m_stepTimer->stop();
    m_stepPhase = StepPhase::Idle;
    m_running = false;
    m_link->cancelQueued();
    if (kind == RunKind::Sweep) {
        emit channelBandRestoreRequested();
        applyBestAndSave();
        if (m_points.isEmpty()) {
            m_measureStatus->setText(outcome);
        } else {
            finishMeasurePdf(true);
            m_measureStatus->setText(outcome + QStringLiteral(" PDF: ") + m_runDir
                                     + QStringLiteral("/report.pdf. CSV: ") + m_runDir
                                     + QStringLiteral("/measurements.csv. Конфиг: ") + productChannelConfigPath());
        }
    } else if (kind == RunKind::Repeat) {
        finishRepeatPdf();
        m_measureStatus->setText(outcome + QStringLiteral(" PDF: ") + m_runDir + QStringLiteral("/repeat.pdf"));
    } else {
        m_measureStatus->setText(outcome);
    }
    ui::setTone(m_measureStatus, ui::Tone::Info);
    m_runKind = RunKind::None;
    m_stopAsked = false;
    setMeasureUi();
}

void FourChannelPanel::continueMeasure()
{
    if (!m_running || m_runKind != RunKind::Sweep) {
        return;
    }
    if (m_stopAsked) {
        stopMeasure(QStringLiteral("Остановлено."));
        return;
    }
    if (m_stepIndex >= m_steps.size()) {
        if (m_allChannels) {
            askNextChannel();
        } else {
            stopMeasure(QStringLiteral("Готово."));
        }
        return;
    }
    applyMeasureStep();
}

void FourChannelPanel::applyMeasureStep()
{
    const int selected = selectedIndex();
    if (m_runKind == RunKind::Sweep) {
        const ChannelStep& step = m_steps.at(m_stepIndex);
        m_phase = m_confirmedPhase;
        m_atten = m_confirmedAtten;
        m_phase[static_cast<std::size_t>(step.channel)] = step.phase;
        m_atten[static_cast<std::size_t>(step.channel)] = step.atten;
        m_phCode = m_phase[static_cast<std::size_t>(selected)];
        m_atCode = m_atten[static_cast<std::size_t>(selected)];
        rememberStep();
        m_measureStatus->setText(QStringLiteral("Канал %1 · фаза %2° · аттенюация %3 дБ · шаг %4 / %5 · жду OK платы")
                                     .arg(step.channel + 1)
                                     .arg(phase_deg(step.phase, m_format.phase), 0, 'f', 1)
                                     .arg(atten_db(step.atten, m_format.atten), 0, 'f', 1)
                                     .arg(m_stepIndex + 1)
                                     .arg(m_steps.size()));
    } else {
        m_phase = m_confirmedPhase;
        m_atten = m_confirmedAtten;
        m_phCode = m_phase[static_cast<std::size_t>(selected)];
        m_atCode = m_atten[static_cast<std::size_t>(selected)];
        m_measureStatus->setText(QStringLiteral("Повтор %1 / %2 · заново ставлю тот же код канала %3")
                                     .arg(m_stepIndex + 1)
                                     .arg(m_repeatTarget)
                                     .arg(selected + 1));
    }
    ui::setTone(m_measureStatus, ui::Tone::Info);
    m_global->setText(QStringLiteral("Канал %1 из 4").arg(selected + 1));
    render();
    refreshRows();
        sendFrame([this](bool ok) {
        if (!m_running || m_stopAsked) {
            return;
        }
        if (!ok) {
            revertConfirmed(QStringLiteral("плата не подтвердила кадр"));
            stopMeasure(QStringLiteral("Плата не подтвердила кадр, проход остановлен."));
            return;
        }
        commitConfirmed();
        m_captureIndex = m_stepIndex;
        m_repeatGot = false;
        m_stepPhase = StepPhase::Settle;
        const int pause = m_runKind == RunKind::Sweep ? m_sweepSettleMs : kSettleMs;
        m_measureStatus->setText(m_measureStatus->text()
                                 + QStringLiteral(" · пауза %1 с, анализатор перестраивается")
                                       .arg(pause / 1000.0, 0, 'f', pause % 1000 == 0 ? 0 : 1));
        m_stepTimer->start(pause);
    });
}

void FourChannelPanel::onStepTimer()
{
    if (!m_running || m_stopAsked) {
        return;
    }
    if (m_stepPhase == StepPhase::Settle) {
        if (m_runKind == RunKind::Sweep) {
            emit channelMarkersRequested(m_stepIndex);
            m_stepPhase = StepPhase::Collect;
            m_measureStatus->setText(m_measureStatus->text() + QStringLiteral(" · читаю маркеры"));
            m_stepTimer->start(kMarkerWaitMs);
            return;
        }
        emit channelSweepRequested();
        m_stepPhase = StepPhase::Collect;
        m_stepTimer->start(kCollectMs);
        return;
    }
    if (m_stepPhase != StepPhase::Collect) {
        return;
    }
    if (m_runKind == RunKind::Sweep) {
        appendLog(QStringLiteral("маркеры не пришли за 20 с, шаг записан без них"));
        saveScreenshot(m_stepIndex);
        finishSweepStep();
        return;
    }
    m_stepPhase = StepPhase::Idle;
    ++m_stepIndex;
    m_measureBar->setValue(m_stepIndex);
    if (m_stepIndex >= m_repeatTarget) {
        stopMeasure(QStringLiteral("Готово."));
        return;
    }
    applyMeasureStep();
}

void FourChannelPanel::rememberStep()
{
    const ChannelStep& step = m_steps.at(m_stepIndex);
    ChannelPoint point;
    point.step = step;
    point.product = m_product != nullptr ? m_product->currentIndex() + 1 : 1;
    point.phaseDeg = phase_deg(step.phase, m_format.phase);
    point.attenDb = atten_db(step.atten, m_format.atten);
    m_points.append(point);
}

void FourChannelPanel::chooseScreenshots()
{
    m_shotSteps = pickScreenshotSteps(m_steps.size(), 10, QRandomGenerator::global()->generate());
    QStringList numbers;
    numbers.reserve(m_shotSteps.size());
    for (const int index : m_shotSteps) {
        numbers.append(QString::number(index + 1));
    }
    appendLog(QStringLiteral("снимки %1 из %2, точки %3")
                  .arg(m_shotSteps.size())
                  .arg(m_steps.size())
                  .arg(numbers.join(QStringLiteral(", "))));
}

void FourChannelPanel::saveScreenshot(int index)
{
    const int row = m_pointBase + index;
    if (index < 0 || row >= m_points.size() || m_runDir.isEmpty()
        || !std::binary_search(m_shotSteps.cbegin(), m_shotSteps.cend(), index)) {
        return;
    }
    QImage shot = captureS2VnaWindow();
    if (shot.isNull()) {
        shot = renderChannelGraph(m_points.at(row));
        appendLog(QStringLiteral("окно S2VNA не найдено, в PDF попал запасной график"));
    }
    if (shot.width() > 1200) {
        shot = shot.scaledToWidth(1200, Qt::SmoothTransformation);
    }
    const QString imagePath = m_runDir + QStringLiteral("/graphs/%1.jpg").arg(row + 1, 5, 10, QLatin1Char('0'));
    shot.save(imagePath, "JPEG", 80);
    m_points[row].imageFile = imagePath;
}


void FourChannelPanel::onSweepFinished(bool ok, const QString& message, const QVector<double>& freqGhz,
                                       const QVector<double>& s11mag, const QVector<double>& s21mag,
                                       const QVector<double>& s12mag, const QVector<double>& s22mag)
{
    const int row = m_pointBase + m_captureIndex;
    if (!m_running || row < 0 || row >= m_points.size()) {
        return;
    }
    if (!ok) {
        appendLog(message.isEmpty() ? QStringLiteral("снимок S-параметров не прочитан") : message);
        return;
    }
    ChannelPoint& point = m_points[row];
    point.freqGhz = freqGhz;
    point.s11mag = s11mag;
    point.s21mag = s21mag;
    point.s12mag = s12mag;
    point.s22mag = s22mag;
    deriveChannelPoint(point);
}

void FourChannelPanel::onLiveTrace(bool ok, const QString& message, int parameter,
                                   const QVector<double>& freqGhz, const QVector<double>& magDb)
{
    Q_UNUSED(freqGhz);
    if (!m_running || m_runKind != RunKind::Repeat || m_stepPhase != StepPhase::Collect) {
        return;
    }
    if (!ok || magDb.isEmpty() || parameter < 0 || parameter > 3) {
        appendLog(message.isEmpty() ? QStringLiteral("текущая трасса не прочитана") : message);
        return;
    }
    if (m_repeatGot) {
        return;
    }
    m_repeatGot = true;
    m_repeatParameter = parameter;
    m_repeatDb.append(channelMidDb(magDb));
}

void FourChannelPanel::finishMeasurePdf(bool openReport)
{
    if (m_runDir.isEmpty() || m_points.isEmpty()) {
        return;
    }
    const QString pdfError = writeChannelReport(m_runDir, m_runTitle, m_points, m_sweepNote);
    const QString csvError = writeChannelCsv(m_runDir, m_points);
    if (!csvError.isEmpty()) {
        appendLog(csvError);
    }
    if (!pdfError.isEmpty()) {
        m_measureStatus->setText(pdfError);
        ui::setTone(m_measureStatus, ui::Tone::Error);
        return;
    }
    if (openReport) {
        showReport(m_runDir + QStringLiteral("/report.pdf"));
    }
}

void FourChannelPanel::finishRepeatPdf()
{
    if (m_runDir.isEmpty()) {
        return;
    }
    const QString parameter = m_repeatParameter == 0 ? QStringLiteral("S11")
                             : m_repeatParameter == 1 ? QStringLiteral("S21")
                             : m_repeatParameter == 2 ? QStringLiteral("S12")
                             : m_repeatParameter == 3 ? QStringLiteral("S22")
                                                      : QStringLiteral("трасса");
    const int selected = selectedIndex();
    const QString error = writeRepeatReport(
        m_runDir, m_runTitle, selected, phase_deg(m_confirmedPhase[static_cast<std::size_t>(selected)], m_format.phase),
        atten_db(m_confirmedAtten[static_cast<std::size_t>(selected)], m_format.atten), parameter, m_repeatDb);
    if (!error.isEmpty()) {
        m_measureStatus->setText(error);
        ui::setTone(m_measureStatus, ui::Tone::Error);
        return;
    }
    showReport(m_runDir + QStringLiteral("/repeat.pdf"));
}

void FourChannelPanel::finishSweepStep()
{
    m_stepTimer->stop();
    m_stepPhase = StepPhase::Idle;
    ++m_stepIndex;
    m_measureBar->setValue(m_stepIndex);
    continueMeasure();
}

void FourChannelPanel::onChannelGrid(bool ok, const QString& message, double stepMhz, int settleMs)
{
    Q_UNUSED(stepMhz);
    if (!m_running || !m_gridPending || m_runKind != RunKind::Sweep) {
        emit channelBandRestoreRequested();
        return;
    }
    m_gridPending = false;
    if (!ok) {
        m_running = false;
        m_runKind = RunKind::None;
        m_allChannels = false;
        m_stopAsked = false;
        const QString text = message.isEmpty() ? QStringLiteral("Не удалось поставить шаг свипа.") : message;
        m_measureStatus->setText(text);
        ui::setTone(m_measureStatus, ui::Tone::Error);
        appendLog(QStringLiteral("измерение не начато: ") + text);
        setMeasureUi();
        QMessageBox::warning(this, QStringLiteral("Измерение не начато"), text);
        return;
    }
    m_sweepSettleMs = settleMs > 0 ? settleMs : m_sweepSettleMs;
    m_sweepNote = message;
    appendLog(message);
    m_measureStatus->setText(message);
    ui::setTone(m_measureStatus, ui::Tone::Info);
    continueMeasure();
}

void FourChannelPanel::onChannelMarkers(bool ok, const QString& message, int stepIndex, double s11, bool s11Ok,
                                       bool s11Swr, double s22, bool s22Ok, bool s22Swr, double s21, bool s21Ok,
                                       double s12, bool s12Ok, double peak, bool peakOk, const QVector<double>& freqHz,
                                       const QVector<double>& gainDb, const QVector<double>& phaseDeg)
{
    if (!m_running || m_runKind != RunKind::Sweep || m_stepPhase != StepPhase::Collect
        || stepIndex != m_captureIndex) {
        return;
    }
    const int row = m_pointBase + m_captureIndex;
    if (row >= 0 && row < m_points.size()) {
        ChannelPoint& point = m_points[row];
        point.s11Db = s11;
        point.s11Ok = s11Ok;
        point.s11Swr = s11Swr;
        point.s22Db = s22;
        point.s22Ok = s22Ok;
        point.s22Swr = s22Swr;
        point.s21Db = s21;
        point.s21Ok = s21Ok;
        point.s12Db = s12;
        point.s12Ok = s12Ok;
        point.peakDb = peak;
        point.peakValid = peakOk;
        point.s21FreqHz = freqHz;
        point.s21GainDb = gainDb;
        point.s21PhaseDeg = phaseDeg;
    }
    if (!ok) {
        appendLog(message.isEmpty() ? QStringLiteral("маркеры не прочитаны") : message);
    } else if (!peakOk) {
        appendLog(QStringLiteral("маркер пика S21 не прочитан"));
    }
    saveScreenshot(m_captureIndex);
    finishSweepStep();
}

void FourChannelPanel::showReport(const QString& path)
{
    if (path.isEmpty() || path == m_shownReport || !QFileInfo::exists(path)) {
        return;
    }
    m_shownReport = path;
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QString FourChannelPanel::reportsRoot() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/channel-reports");
}

QString FourChannelPanel::latestReportDir() const
{
    const auto hasReport = [](const QDir& dir) {
        return dir.exists(QStringLiteral("report.pdf")) || dir.exists(QStringLiteral("measurements.csv"))
               || dir.exists(QStringLiteral("repeat.pdf"));
    };
    if (!m_runDir.isEmpty() && hasReport(QDir(m_runDir))) {
        return m_runDir;
    }
    const QDir root(reportsRoot());
    const QStringList names = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (int i = names.size() - 1; i >= 0; --i) {
        const QDir dir(root.filePath(names.at(i)));
        if (hasReport(dir)) {
            return dir.absolutePath();
        }
    }
    return root.absolutePath();
}

void FourChannelPanel::openReportFiles()
{
    const QString dir = latestReportDir();
    const QString pdf = dir + QStringLiteral("/report.pdf");
    const QString csv = dir + QStringLiteral("/measurements.csv");
    const bool hasPdf = QFileInfo::exists(pdf);
    const bool hasCsv = QFileInfo::exists(csv);
    QMenu menu(this);
    QAction* both = menu.addAction(QStringLiteral("PDF и CSV"));
    both->setEnabled(hasPdf || hasCsv);
    QAction* pdfAction = menu.addAction(QStringLiteral("PDF"));
    pdfAction->setEnabled(hasPdf);
    QAction* csvAction = menu.addAction(QStringLiteral("CSV"));
    csvAction->setEnabled(hasCsv);
    menu.addSeparator();
    QAction* folder = menu.addAction(QStringLiteral("Папка отчёта"));
    folder->setEnabled(QDir(dir).exists());
    QAction* other = menu.addAction(QStringLiteral("Выбрать файл…"));
    QAction* chosen = menu.exec(m_openReport->mapToGlobal(QPoint(0, m_openReport->height())));
    if (chosen == nullptr) {
        return;
    }
    const auto open = [](const QString& path) {
        if (QFileInfo::exists(path)) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    };
    if (chosen == both) {
        open(pdf);
        open(csv);
    } else if (chosen == pdfAction) {
        open(pdf);
    } else if (chosen == csvAction) {
        open(csv);
    } else if (chosen == folder) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    } else if (chosen == other) {
        const QString start = QDir(dir).exists() ? dir : reportsRoot();
        const QStringList picked = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Открыть отчёт"), start,
            QStringLiteral("Отчёты (*.pdf *.csv);;PDF (*.pdf);;CSV (*.csv)"));
        for (const QString& path : picked) {
            open(path);
        }
    }
}

void FourChannelPanel::askNextChannel()
{
    if (!m_running || m_runKind != RunKind::Sweep) {
        return;
    }
    const int done = selectedIndex();
    applyBestAndSave();
    finishMeasurePdf(false);
    if (done >= kChannels - 1) {
        stopMeasure(QStringLiteral("Готово, все 4 канала."));
        return;
    }
    const int next = done + 1;
    const int best = bestPowerIndex(m_points, done);
    QString detail = QStringLiteral("Канал %1 записан в общий PDF.\n%2/report.pdf\n"
                                    "Файл откроется, когда остановите проход или закончите все каналы.")
                         .arg(done + 1)
                         .arg(m_runDir);
    if (best >= 0) {
        const ChannelPoint& peak = m_points.at(best);
        detail += QStringLiteral("\nМаксимум S21 %1 дБ при фазе %2° и аттенюации %3 дБ. "
                                 "Эти коды уже стоят на канале и записаны в конфиг изделия.")
                      .arg(peak.peakDb, 0, 'f', 1)
                      .arg(peak.phaseDeg, 0, 'f', 1)
                      .arg(peak.attenDb, 0, 'f', 1);
    }
    detail += QStringLiteral("\nПерекрутите кабель на канал %1 и продолжите, либо остановитесь. "
                             "Уже снятое сохранено.")
                  .arg(next + 1);
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("Канал %1 готов").arg(done + 1));
    box.setText(detail);
    auto* goOn = box.addButton(QStringLiteral("Продолжить, кабель на канал %1").arg(next + 1),
                               QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Сохранить PDF и пауза"), QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != goOn || !m_running) {
        stopMeasure(QStringLiteral("Пауза. Канал %1 сохранён.").arg(done + 1));
        return;
    }
    {
        const QSignalBlocker block(m_slot);
        m_slot->setCurrentIndex(next);
    }
    saveSelection();
    showSelected(false);
    m_steps = buildChannelSteps({next}, max_code(m_format.phase.width), max_code(m_format.atten.width),
                                m_phase, m_atten);
    if (m_steps.isEmpty()) {
        stopMeasure(QStringLiteral("Нет шагов для канала %1.").arg(next + 1));
        return;
    }
    chooseScreenshots();
    m_pointBase = m_points.size();
    m_stepIndex = 0;
    m_captureIndex = -1;
    m_measureBar->setRange(0, m_steps.size());
    m_measureBar->setValue(0);
    m_measureStatus->setText(QStringLiteral("Канал %1. Перекрутите кабель и дождитесь шагов.").arg(next + 1));
    setMeasureUi();
    continueMeasure();
}

void FourChannelPanel::applyBestAndSave()
{
    const int best = bestPowerIndex(m_points, selectedIndex());
    if (best < 0) {
        return;
    }
    const ChannelPoint& point = m_points.at(best);
    const int channel = point.step.channel;
    if (channel < 0 || channel >= kChannels) {
        return;
    }
    const auto index = static_cast<std::size_t>(channel);
    m_phase[index] = point.step.phase;
    m_atten[index] = point.step.atten;
    m_confirmedPhase[index] = point.step.phase;
    m_confirmedAtten[index] = point.step.atten;
    if (selectedIndex() == channel) {
        m_phCode = point.step.phase;
        m_atCode = point.step.atten;
    }
    saveChannel(channel);
    render();
    ProductChannelSetting row;
    row.product = m_product != nullptr ? m_product->currentIndex() + 1 : 1;
    row.channel = channel + 1;
    row.phaseCode = point.step.phase;
    row.attenCode = point.step.atten;
    row.phaseDeg = point.phaseDeg;
    row.attenDb = point.attenDb;
    row.peakDb = point.peakDb;
    row.kuDb = point.kuDb;
    row.vswr = point.vswr;
    row.saved = true;
    QString error;
    if (!saveProductChannel(row, &error)) {
        appendLog(error);
    } else {
        appendLog(QStringLiteral("изделие %1 канал %2: фаза %3°, аттенюация %4 дБ, пик %5 дБ → %6")
                      .arg(row.product)
                      .arg(row.channel)
                      .arg(row.phaseDeg, 0, 'f', 1)
                      .arg(row.attenDb, 0, 'f', 1)
                      .arg(row.peakDb, 0, 'f', 1)
                      .arg(productChannelConfigPath()));
    }
    if (m_connected && selectedIndex() == channel) {
        sendFrame([this](bool ok) {
            if (ok) {
                commitConfirmed();
            }
        });
    }
}

void FourChannelPanel::restoreProduct()
{
    if (m_running || m_product == nullptr) {
        return;
    }
    const int product = m_product->currentIndex() + 1;
    const std::array<ProductChannelSetting, 4> rows = loadProductChannels(product);
    int restored = 0;
    for (int i = 0; i < kChannels; ++i) {
        const ProductChannelSetting& row = rows[static_cast<std::size_t>(i)];
        if (!row.saved) {
            continue;
        }
        m_phase[static_cast<std::size_t>(i)] = clamp_code(row.phaseCode, m_format.phase);
        m_atten[static_cast<std::size_t>(i)] = clamp_code(row.attenCode, m_format.atten);
        saveChannel(i);
        ++restored;
    }
    showSelected(false);
    if (restored == 0) {
        m_measureStatus->setText(QStringLiteral("В конфиге изделия %1 ещё нет сохранённых каналов.").arg(product));
        ui::setTone(m_measureStatus, ui::Tone::Warn);
        return;
    }
    if (m_connected) {
        queueSend(true);
    }
    m_measureStatus->setText(QStringLiteral("Изделие %1: восстановлено каналов %2. Файл %3")
                                 .arg(product)
                                 .arg(restored)
                                 .arg(productChannelConfigPath()));
    ui::setTone(m_measureStatus, ui::Tone::Ok);
}

void FourChannelPanel::setMeasureUi()
{
    const bool idle = !m_running;
    if (m_readBoard != nullptr) {
        m_readBoard->setEnabled(idle && m_connected);
    }
    m_measureStart->setEnabled(idle);
    if (m_measureAll != nullptr) {
        m_measureAll->setEnabled(idle);
    }
    if (m_product != nullptr) {
        m_product->setEnabled(idle);
    }
    if (m_restoreProduct != nullptr) {
        m_restoreProduct->setEnabled(idle);
    }
    m_repeatStart->setEnabled(idle);
    m_repeatCount->setEnabled(idle);
    m_measureStop->setEnabled(m_running);
    m_slot->setEnabled(idle);
    for (QPushButton* row : m_rows) {
        if (row != nullptr) {
            row->setEnabled(idle);
        }
    }
    m_measureBar->setVisible(m_running || m_measureBar->value() > 0);
    setEditEnabled();
}

void FourChannelPanel::appendLog(const QString& line)
{
    m_log->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz  ")) + line);
}
