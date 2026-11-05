#include "FourChannelPanel.h"

#include "Ppkh4Frame.h"
#include "Ppkh4Link.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
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
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace {

using namespace afar::ppkh4;

constexpr int kLogLines = 200;
constexpr int kSendDebounceMs = 60;
constexpr int kChannels = 16;
constexpr int kPerBoard = 4;

QString key(const char* name)
{
    return QStringLiteral("fourChannel/") + QLatin1String(name);
}

QString channelKey(const char* field, int index)
{
    return QStringLiteral("fourChannel/%1/%2").arg(QLatin1String(field)).arg(index);
}

/// Раскладка кадра — та же, что на вкладке «Настройка 1 канала».
void readSharedFormat(FrameFormat& format, LineConfig& lines)
{
    QSettings s;
    const FrameFormat d;
    const LineConfig l;
    auto raw = [&](const char* name, const QVariant& fallback) {
        return s.value(QStringLiteral("singleChannel/") + QLatin1String(name), fallback);
    };
    format.total_bits = raw("totalBits", d.total_bits).toInt();
    format.fixed_hex = raw("fixedHex", QString::fromStdString(d.fixed_hex)).toString().toStdString();
    format.phase.offset = raw("phOff", d.phase.offset).toInt();
    format.phase.width = raw("phW", d.phase.width).toInt();
    format.phase.lsb = raw("phLsb", d.phase.lsb).toDouble();
    format.phase.invert = raw("phInv", d.phase.invert).toBool();
    format.phase.reverse = raw("phRev", d.phase.reverse).toBool();
    format.atten.offset = raw("atOff", d.atten.offset).toInt();
    format.atten.width = raw("atW", d.atten.width).toInt();
    format.atten.lsb = raw("atLsb", d.atten.lsb).toDouble();
    format.atten.invert = raw("atInv", d.atten.invert).toBool();
    format.atten.reverse = raw("atRev", d.atten.reverse).toBool();
    lines.lsb_first = raw("lsbFirst", l.lsb_first).toInt();
    lines.clk_idle = raw("clkIdle", l.clk_idle).toInt();
    lines.le_active = raw("leActive", l.le_active).toInt();
    lines.le_mode = raw("leMode", l.le_mode).toInt();
    lines.half_us = raw("halfUs", l.half_us).toInt();
    lines.le_us = raw("leUs", l.le_us).toInt();
    if (!format_valid(format)) {
        format = d;
        lines = l;
    }
}

QString codeText(int code, const FieldFormat& field, bool phase)
{
    const double value = phase ? phase_deg(code, field) : atten_db(code, field);
    const int digits = phase ? 3 : 2;
    const QString unit = phase ? QStringLiteral("°") : QStringLiteral(" дБ");
    return QString::number(value, 'f', digits) + unit;
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

int FourChannelPanel::boardIndex() const
{
    return m_board->currentIndex();
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
    return m_board->currentIndex() * kPerBoard + m_slot->currentIndex();
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
    auto* box = new QGroupBox(QStringLiteral("Изделие и канал на кабелях C2220"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);

    auto* pick = new QHBoxLayout();
    m_board = new QComboBox(box);
    m_board->setObjectName(QStringLiteral("fourChannelBoard"));
    m_slot = new QComboBox(box);
    m_slot->setObjectName(QStringLiteral("fourChannelSlot"));
    for (int i = 1; i <= kPerBoard; ++i) {
        m_board->addItem(QStringLiteral("Изделие %1").arg(i));
        m_slot->addItem(QStringLiteral("Канал %1").arg(i));
    }
    m_global = new QLabel(box);
    m_global->setObjectName(QStringLiteral("fourChannelGlobal"));
    m_apply = new QPushButton(QStringLiteral("Выставить сохранённое"), box);
    m_apply->setObjectName(QStringLiteral("fourChannelApply"));
    ui::setButtonRole(m_apply, ui::ButtonRole::Primary);
    pick->addWidget(ui::captionLabel(QStringLiteral("Изделие"), box));
    pick->addWidget(m_board);
    pick->addWidget(ui::captionLabel(QStringLiteral("Канал"), box));
    pick->addWidget(m_slot);
    pick->addWidget(m_global, 1);
    pick->addWidget(m_apply);
    lay->addLayout(pick);

    lay->addWidget(ui::hintLabel(
        QStringLiteral("У C2220 один вход и один выход. Переставьте кабели на нужный канал и выберите его здесь. "
                       "Программа выставит сохранённые фазу и аттенюацию. Коды хранятся в программе: "
                       "прошивка ППКХ4 номер изделия и каналы не запоминает. Раскладка кадра берётся "
                       "из вкладки «Настройка 1 канала». Если порт занят той вкладкой, сначала отключите её."),
        box));

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(ui::kGap);
    grid->setVerticalSpacing(4);
    for (int i = 0; i < kPerBoard; ++i) {
        auto* row = new QPushButton(box);
        row->setObjectName(QStringLiteral("fourChannelRow%1").arg(i));
        row->setCheckable(true);
        row->setAutoExclusive(true);
        ui::setButtonRole(row, ui::ButtonRole::Secondary);
        m_rows[static_cast<std::size_t>(i)] = row;
        grid->addWidget(row, i, 0);
        connect(row, &QPushButton::clicked, this, [this, i]() {
            if (m_slot->currentIndex() == i) {
                if (m_connected) {
                    sendFrame();
                }
            } else {
                m_slot->setCurrentIndex(i);
            }
        });
    }
    lay->addLayout(grid);

    connect(m_board, &QComboBox::currentIndexChanged, this, &FourChannelPanel::onSelectionChanged);
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
    readSharedFormat(m_format, m_lines);
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
    }
    const int board = std::clamp(s.value(key("board"), 0).toInt(), 0, kPerBoard - 1);
    const int slot = std::clamp(s.value(key("slot"), 0).toInt(), 0, kPerBoard - 1);
    m_board->setCurrentIndex(board);
    m_slot->setCurrentIndex(slot);
    m_port->setEditText(s.value(key("port")).toString());
    m_loading = false;
}

void FourChannelPanel::saveSelection() const
{
    QSettings s;
    s.setValue(key("board"), m_board->currentIndex());
    s.setValue(key("slot"), m_slot->currentIndex());
}

void FourChannelPanel::saveChannel(int index) const
{
    QSettings s;
    s.setValue(channelKey("phase", index), m_phase[static_cast<std::size_t>(index)]);
    s.setValue(channelKey("atten", index), m_atten[static_cast<std::size_t>(index)]);
    s.setValue(key("board"), m_board->currentIndex());
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
    m_global->setText(QStringLiteral("Изделие %1 · канал %2")
                          .arg(m_board->currentIndex() + 1)
                          .arg(m_slot->currentIndex() + 1));
    render();
    refreshRows();
    if (sendNow && m_connected) {
        sendFrame();
    }
}

void FourChannelPanel::refreshRows()
{
    const int slot = m_slot->currentIndex();
    const int board = m_board->currentIndex();
    for (int i = 0; i < kPerBoard; ++i) {
        const int index = board * kPerBoard + i;
        const int phase = m_phase[static_cast<std::size_t>(index)];
        const int atten = m_atten[static_cast<std::size_t>(index)];
        auto* row = m_rows[static_cast<std::size_t>(i)];
        row->setText(QStringLiteral("Изделие %1 · канал %2 · фаза %3 · аттенюация %4")
                         .arg(board + 1)
                         .arg(i + 1)
                         .arg(codeText(phase, m_format.phase, true))
                         .arg(codeText(atten, m_format.atten, false)));
        const QSignalBlocker block(row);
        row->setChecked(i == slot);
    }
}

void FourChannelPanel::setPhaseCode(int code)
{
    const int next = clamp_code(code, m_format.phase);
    if (next == m_phCode) {
        return;
    }
    m_phCode = next;
    const int index = selectedIndex();
    m_phase[static_cast<std::size_t>(index)] = next;
    saveChannel(index);
    render();
    refreshRows();
    if (m_connected) {
        m_sendDebounce->start();
    }
}

void FourChannelPanel::setAttenCode(int code)
{
    const int next = clamp_code(code, m_format.atten);
    if (next == m_atCode) {
        return;
    }
    m_atCode = next;
    const int index = selectedIndex();
    m_atten[static_cast<std::size_t>(index)] = next;
    saveChannel(index);
    render();
    refreshRows();
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
    const auto bits = build_frame(m_format, m_phCode, m_atCode);
    const QString hex = QString::fromStdString(frame_hex(bits));
    const int product = m_board->currentIndex() + 1;
    const int channel = m_slot->currentIndex() + 1;
    const int phase = m_phCode;
    const int atten = m_atCode;
    const QString command = QString::fromStdString(send_command(m_format, phase, atten));
    m_link->send(command, [this, hex, product, channel, phase, atten](const QString& reply) {
        if (reply.startsWith(QLatin1String("OK"))) {
            m_lastSent->setText(QStringLiteral("изделие %1, канал %2 записан: %3, %4 (0x%5)")
                                    .arg(product)
                                    .arg(channel)
                                    .arg(codeText(phase, m_format.phase, true))
                                    .arg(codeText(atten, m_format.atten, false))
                                    .arg(hex));
            ui::setTone(m_lastSent, ui::Tone::Ok);
        } else if (reply.startsWith(QLatin1String("ERR"))) {
            m_lastSent->setText(QStringLiteral("изделие %1, канал %2: %3").arg(product).arg(channel).arg(reply));
            ui::setTone(m_lastSent, ui::Tone::Error);
        }
    });
}

void FourChannelPanel::appendLog(const QString& line)
{
    m_log->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz  ")) + line);
}
