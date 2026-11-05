#include "FourChannelPanel.h"

#include "Ppkh4Frame.h"
#include "Ppkh4Link.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <vector>

#include <algorithm>

namespace {

using namespace afar::ppkh4;

constexpr int kLogLines = 200;
constexpr int kSendDebounceMs = 60;
constexpr int kChannels = 4;

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

FourChannelPanel::FourChannelPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("fourChannelPanel"));
    m_link = new Ppkh4Link(this);
    m_sendDebounce = new QTimer(this);
    m_sendDebounce->setSingleShot(true);
    m_sendDebounce->setInterval(kSendDebounceMs);

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
    root->addWidget(makeLogBox());

    connect(m_sendDebounce, &QTimer::timeout, this, &FourChannelPanel::sendFrame);
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
    pick->addWidget(ui::captionLabel(QStringLiteral("Канал"), box));
    pick->addWidget(m_slot);
    pick->addWidget(m_global, 1);
    pick->addWidget(m_apply);
    lay->addLayout(pick);

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
            if (m_slot->currentIndex() == i) {
                if (m_connected) {
                    sendFrame();
                }
            } else {
                m_slot->setCurrentIndex(i);
            }
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
    connect(m_apply, &QPushButton::clicked, this, &FourChannelPanel::sendFrame);
    return box;
}

QWidget* FourChannelPanel::makePhaseBox()
{
    auto* box = new QGroupBox(QStringLiteral("Фаза выбранного канала"), this);
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
        sendFrame();
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
    }
    const int slot = std::clamp(s.value(key("slot"), 0).toInt(), 0, kChannels - 1);
    m_slot->setCurrentIndex(slot);
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
    if (m_loading) {
        return;
    }
    saveSelection();
    showSelected(m_connected);
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
        sendFrame();
    }
}

void FourChannelPanel::refreshRows()
{
    const int slot = m_slot->currentIndex();
    for (int i = 0; i < kChannels; ++i) {
        const int phase = m_phase[static_cast<std::size_t>(i)];
        const int atten = m_atten[static_cast<std::size_t>(i)];
        auto* row = m_rows[static_cast<std::size_t>(i)];
        QString text = QStringLiteral("Канал %1 · фаза %2 · аттенюация %3")
                           .arg(i + 1)
                           .arg(codeText(phase, m_format.phase, true))
                           .arg(codeText(atten, m_format.atten, false));
        if (m_lsb[static_cast<std::size_t>(i)] != 0) {
            text += QStringLiteral(" · младшим вперёд");
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
    if (m_loading || index < 0 || index >= kChannels) {
        return;
    }
    const int next = clamp_code(code, m_format.phase);
    const bool switchTo = index != selectedIndex();
    if (!switchTo && m_phase[static_cast<std::size_t>(index)] == next) {
        return;
    }
    if (switchTo) {
        const QSignalBlocker block(m_slot);
        m_slot->setCurrentIndex(index);
    }
    m_phase[static_cast<std::size_t>(index)] = next;
    m_phCode = m_phase[static_cast<std::size_t>(selectedIndex())];
    m_atCode = m_atten[static_cast<std::size_t>(selectedIndex())];
    m_global->setText(QStringLiteral("Канал %1 из 4").arg(selectedIndex() + 1));
    saveChannel(index);
    render();
    if (m_connected) {
        m_sendDebounce->start();
    }
}

void FourChannelPanel::setChannelAtten(int index, int code)
{
    if (m_loading || index < 0 || index >= kChannels) {
        return;
    }
    const int next = clamp_code(code, m_format.atten);
    const bool switchTo = index != selectedIndex();
    if (!switchTo && m_atten[static_cast<std::size_t>(index)] == next) {
        return;
    }
    if (switchTo) {
        const QSignalBlocker block(m_slot);
        m_slot->setCurrentIndex(index);
    }
    m_atten[static_cast<std::size_t>(index)] = next;
    m_phCode = m_phase[static_cast<std::size_t>(selectedIndex())];
    m_atCode = m_atten[static_cast<std::size_t>(selectedIndex())];
    m_global->setText(QStringLiteral("Канал %1 из 4").arg(selectedIndex() + 1));
    saveChannel(index);
    render();
    if (m_connected) {
        m_sendDebounce->start();
    }
}

void FourChannelPanel::setChannelOrder(int index, int lsbFirst)
{
    if (m_loading || index < 0 || index >= kChannels) {
        return;
    }
    const int next = lsbFirst != 0 ? 1 : 0;
    if (m_lsb[static_cast<std::size_t>(index)] == next) {
        return;
    }
    if (index != selectedIndex()) {
        const QSignalBlocker block(m_slot);
        m_slot->setCurrentIndex(index);
        m_phCode = m_phase[static_cast<std::size_t>(index)];
        m_atCode = m_atten[static_cast<std::size_t>(index)];
        m_global->setText(QStringLiteral("Канал %1 из 4").arg(index + 1));
    }
    m_lsb[static_cast<std::size_t>(index)] = next;
    saveChannel(index);
    render();
    if (m_connected) {
        m_sendDebounce->start();
    }
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
    m_phRange->setRange(0, phMax);
    m_phNum->setRange(0, phMax);
    m_atRange->setRange(0, atMax);
    m_atNum->setRange(0, atMax);
    m_phRange->setValue(m_phCode);
    m_atRange->setValue(m_atCode);
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

void FourChannelPanel::refreshPacket()
{
    const auto bits = chain_bits(m_format, m_phase, m_atten, m_lsb);
    if (m_packet != nullptr) {
        m_packet->setText(QString::fromStdString(
            send_chain_command(m_format, m_phase, m_atten, m_lsb)));
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
        if (m_sidePhaseSlider[index] != nullptr) {
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
        if (m_sideAttenSlider[index] != nullptr) {
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
        connect(phaseCode, &QSpinBox::valueChanged, this,
                [this, i](int value) { setChannelPhase(i, value); });
        connect(phaseDeg, &QDoubleSpinBox::editingFinished, this, [this, i, phaseDeg]() {
            setChannelPhase(i, phase_code_from_deg(phaseDeg->value(), m_format.phase));
        });
        connect(attenSlider, &QSlider::valueChanged, this,
                [this, i](int value) { setChannelAtten(i, value); });
        connect(attenCode, &QSpinBox::valueChanged, this,
                [this, i](int value) { setChannelAtten(i, value); });
        connect(attenDb, &QDoubleSpinBox::editingFinished, this, [this, i, attenDb]() {
            setChannelAtten(i, atten_code_from_db(attenDb->value(), m_format.atten));
        });
    }
    m_side = box;
}

void FourChannelPanel::applyCfg()
{
    for (const auto& cmd : cfg_commands(m_lines)) {
        m_link->send(QString::fromStdString(cmd));
    }
}

void FourChannelPanel::sendFrame()
{
    m_sendDebounce->stop();
    if (!m_connected) {
        return;
    }
    const QString command = QString::fromStdString(
        send_chain_command(m_format, m_phase, m_atten, m_lsb));
    const int channel = m_slot->currentIndex() + 1;
    m_link->send(command, [this, command, channel](const QString& reply) {
        if (reply.startsWith(QLatin1String("OK"))) {
            m_lastSent->setText(QStringLiteral("цепочка записана, правится канал %1: %2")
                                    .arg(channel)
                                    .arg(command));
            ui::setTone(m_lastSent, ui::Tone::Ok);
        } else if (reply.startsWith(QLatin1String("ERR"))) {
            m_lastSent->setText(QStringLiteral("канал %1: %2").arg(channel).arg(reply));
            ui::setTone(m_lastSent, ui::Tone::Error);
        }
    });
}

void FourChannelPanel::appendLog(const QString& line)
{
    m_log->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz  ")) + line);
}
