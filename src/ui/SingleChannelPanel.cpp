#include "SingleChannelPanel.h"

#include "Ppkh4Link.h"
#include "UiStyle.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

using namespace afar::ppkh4;

// Цвета полей — как в исходной странице 1Chanel; читаются на светлой и тёмной теме.
constexpr auto kPhaseColor = "#7c3aed";
constexpr auto kAttenColor = "#0e7490";
constexpr int kLogLines = 300;
constexpr int kSendDebounceMs = 60;

QString settingsKey(const char* name)
{
    return QStringLiteral("singleChannel/") + QLatin1String(name);
}

QLabel* bigValue(QWidget* parent)
{
    auto* label = new QLabel(parent);
    // Шрифт задаёт таблица стилей темы, поэтому размер — тоже стилем.
    label->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 600;"));
    return label;
}

QString binary(int code, int width)
{
    return QStringLiteral("0b") + QString::number(code, 2).rightJustified(width, QLatin1Char('0'));
}

}  // namespace

SingleChannelPanel::SingleChannelPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("singleChannelPanel"));
    m_link = new Ppkh4Link(this);
    m_sendDebounce = new QTimer(this);
    m_sendDebounce->setSingleShot(true);
    m_sendDebounce->setInterval(kSendDebounceMs);
    m_sweepTimer = new QTimer(this);
    m_sweepTimer->setSingleShot(true);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(ui::kGap);

    root->addWidget(makeConnectionBox());
    auto* row = new QHBoxLayout();
    row->setSpacing(ui::kGap);
    row->addWidget(makePhaseBox(), 1);
    row->addWidget(makeAttenBox(), 1);
    row->addWidget(makeSweepBox(), 1);
    root->addLayout(row);
    root->addWidget(makeFormatBox());
    root->addWidget(makeLogBox());

    connect(m_sendDebounce, &QTimer::timeout, this, [this]() { sendFrame(); });
    connect(m_sweepTimer, &QTimer::timeout, this, &SingleChannelPanel::sweepTick);
    connect(m_link, &Ppkh4Link::opened, this, &SingleChannelPanel::onOpened);
    connect(m_link, &Ppkh4Link::closed, this, &SingleChannelPanel::onClosed);
    connect(m_link, &Ppkh4Link::commandSent, this,
            [this](const QString& cmd) { appendLog(QStringLiteral("→ ") + cmd); });
    connect(m_link, &Ppkh4Link::lineReceived, this,
            [this](const QString& line) { appendLog(QStringLiteral("← ") + line); });
    connect(m_link, &Ppkh4Link::boardError, this, [this](const QString& text) {
        setConnState(QStringLiteral("ошибка платы: ") + text, false, true);
    });

    loadSettings();
    loadForm();
    refreshPorts();
    render();
    updateButtons();
}

SingleChannelPanel::~SingleChannelPanel()
{
    m_sweepTimer->stop();
    m_sendDebounce->stop();
}

QString SingleChannelPanel::sendCommand() const
{
    return QString::fromStdString(send_command(m_format, m_phCode, m_atCode));
}

QWidget* SingleChannelPanel::createQuickControls()
{
    auto* box = new QGroupBox(QStringLiteral("Настройка 1 канала"));
    box->setObjectName(QStringLiteral("quickSingleChannelControls"));
    box->setToolTip(QStringLiteral(
        "Использует соединение с ППКХ4 из верхней вкладки «Настройка 1 канала». "
        "Без подключения значения меняются только на экране."));
    auto* layout = new QVBoxLayout(box);
    layout->addWidget(new QLabel(QStringLiteral("Фаза"), box));
    m_quickPhRange = new QSlider(Qt::Horizontal, box);
    m_quickPhRange->setObjectName(QStringLiteral("quickChannelPhaseSlider"));
    layout->addWidget(m_quickPhRange);
    auto* phaseSteps = new QHBoxLayout();
    auto* phaseDown = new QPushButton(QStringLiteral("−1 шаг"), box);
    phaseDown->setObjectName(QStringLiteral("quickChannelPhaseDown"));
    auto* phaseUp = new QPushButton(QStringLiteral("+1 шаг"), box);
    phaseUp->setObjectName(QStringLiteral("quickChannelPhaseUp"));
    phaseSteps->addWidget(phaseDown);
    phaseSteps->addWidget(phaseUp);
    layout->addLayout(phaseSteps);
    m_quickPhDeg = new QDoubleSpinBox(box);
    m_quickPhDeg->setObjectName(QStringLiteral("quickChannelPhaseValue"));
    m_quickPhDeg->setDecimals(3);
    m_quickPhDeg->setSuffix(QStringLiteral("°"));
    layout->addWidget(m_quickPhDeg);
    layout->addWidget(new QLabel(QStringLiteral("Аттенюация"), box));
    m_quickAtRange = new QSlider(Qt::Horizontal, box);
    m_quickAtRange->setObjectName(QStringLiteral("quickChannelAttenSlider"));
    layout->addWidget(m_quickAtRange);
    auto* attenSteps = new QHBoxLayout();
    auto* attenDown = new QPushButton(QStringLiteral("−0.5 дБ"), box);
    attenDown->setObjectName(QStringLiteral("quickChannelAttenDown"));
    auto* attenUp = new QPushButton(QStringLiteral("+0.5 дБ"), box);
    attenUp->setObjectName(QStringLiteral("quickChannelAttenUp"));
    attenSteps->addWidget(attenDown);
    attenSteps->addWidget(attenUp);
    layout->addLayout(attenSteps);
    m_quickAtDb = new QDoubleSpinBox(box);
    m_quickAtDb->setObjectName(QStringLiteral("quickChannelAttenValue"));
    m_quickAtDb->setDecimals(2);
    m_quickAtDb->setSuffix(QStringLiteral(" дБ"));
    layout->addWidget(m_quickAtDb);

    connect(m_quickPhRange, &QSlider::valueChanged, this, &SingleChannelPanel::setPhaseCode);
    connect(phaseDown, &QPushButton::clicked, this, [this]() { setPhaseCode(m_phCode - 1); });
    connect(phaseUp, &QPushButton::clicked, this, [this]() { setPhaseCode(m_phCode + 1); });
    connect(m_quickPhDeg, &QDoubleSpinBox::editingFinished, this,
            [this]() { setPhaseCode(phase_code_from_deg(m_quickPhDeg->value(), m_format.phase)); });
    connect(m_quickAtRange, &QSlider::valueChanged, this, &SingleChannelPanel::setAttenCode);
    connect(attenDown, &QPushButton::clicked, this, [this]() { setAttenCode(m_atCode - 1); });
    connect(attenUp, &QPushButton::clicked, this, [this]() { setAttenCode(m_atCode + 1); });
    connect(m_quickAtDb, &QDoubleSpinBox::editingFinished, this,
            [this]() { setAttenCode(atten_code_from_db(m_quickAtDb->value(), m_format.atten)); });
    render();
    updateButtons();
    return box;
}

void SingleChannelPanel::setPhaseCode(int code)
{
    const int next = clamp_code(code, m_format.phase);
    if (next == m_phCode) return;
    m_phCode = next;
    render();
    changed();
}

void SingleChannelPanel::setAttenCode(int code)
{
    const int next = clamp_code(code, m_format.atten);
    if (next == m_atCode) return;
    m_atCode = next;
    render();
    changed();
}

// --- Построение ---

QWidget* SingleChannelPanel::makeConnectionBox()
{
    auto* box = new QGroupBox(QStringLiteral("Плата ППКХ4 · канал X2"), this);
    auto* lay = new QHBoxLayout(box);
    ui::applyGroupLayout(lay);

    m_port = new QComboBox(box);
    m_port->setObjectName(QStringLiteral("singleChannelPort"));
    m_port->setEditable(true);
    m_port->setMinimumContentsLength(14);
    m_port->setToolTip(QStringLiteral("Виртуальный COM-порт платы (USB, VID 0483 / PID 5740)"));
    m_refresh = new QPushButton(QStringLiteral("Обновить"), box);
    ui::setButtonRole(m_refresh, ui::ButtonRole::Secondary);
    m_connect = new QPushButton(QStringLiteral("Подключить"), box);
    m_connect->setObjectName(QStringLiteral("singleChannelConnect"));
    ui::setButtonRole(m_connect, ui::ButtonRole::Primary);
    m_conn = new QLabel(box);
    m_conn->setProperty("role", QStringLiteral("status"));
    m_lastSent = ui::hintLabel(QString(), box);
    m_lastSent->setObjectName(QStringLiteral("singleChannelLastSent"));

    lay->addWidget(ui::captionLabel(QStringLiteral("COM-порт"), box));
    lay->addWidget(m_port);
    lay->addWidget(m_refresh);
    lay->addWidget(m_connect);
    lay->addWidget(m_conn);
    lay->addWidget(m_lastSent, 1);

    connect(m_refresh, &QPushButton::clicked, this, &SingleChannelPanel::refreshPorts);
    connect(m_connect, &QPushButton::clicked, this, &SingleChannelPanel::toggleConnection);
    setConnState(Ppkh4Link::serialSupported()
                     ? QStringLiteral("не подключено")
                     : QStringLiteral("нет модуля Qt Serial Port"),
                 false, !Ppkh4Link::serialSupported());
    return box;
}

QWidget* SingleChannelPanel::makePhaseBox()
{
    auto* box = new QGroupBox(QStringLiteral("Фаза"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);

    auto* head = new QHBoxLayout();
    m_phVal = bigValue(box);
    m_phVal->setObjectName(QStringLiteral("singleChannelPhaseValue"));
    m_phCodeLabel = ui::hintLabel(QString(), box);
    head->addWidget(m_phVal);
    head->addWidget(m_phCodeLabel, 1);
    lay->addLayout(head);

    m_phRange = new QSlider(Qt::Horizontal, box);
    lay->addWidget(m_phRange);

    auto* row = new QHBoxLayout();
    auto* dn = new QPushButton(QStringLiteral("−1 шаг"), box);
    auto* up = new QPushButton(QStringLiteral("+1 шаг"), box);
    m_phDeg = new QDoubleSpinBox(box);
    m_phDeg->setDecimals(3);
    m_phDeg->setRange(0.0, 360000.0);
    m_phDeg->setSuffix(QStringLiteral("°"));
    m_phNum = new QSpinBox(box);
    m_phNum->setObjectName(QStringLiteral("singleChannelPhaseCode"));
    row->addWidget(dn);
    row->addWidget(up);
    row->addWidget(ui::captionLabel(QStringLiteral("градусы"), box));
    row->addWidget(m_phDeg);
    row->addWidget(ui::captionLabel(QStringLiteral("код"), box));
    row->addWidget(m_phNum);
    row->addStretch(1);
    lay->addLayout(row);

    connect(m_phRange, &QSlider::valueChanged, this, &SingleChannelPanel::setPhaseCode);
    connect(m_phNum, &QSpinBox::valueChanged, this, &SingleChannelPanel::setPhaseCode);
    connect(m_phDeg, &QDoubleSpinBox::editingFinished, this,
            [this]() { setPhaseCode(phase_code_from_deg(m_phDeg->value(), m_format.phase)); });
    connect(up, &QPushButton::clicked, this, [this]() { setPhaseCode(m_phCode + 1); });
    connect(dn, &QPushButton::clicked, this, [this]() { setPhaseCode(m_phCode - 1); });
    return box;
}

QWidget* SingleChannelPanel::makeAttenBox()
{
    auto* box = new QGroupBox(QStringLiteral("Аттенюация"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);

    auto* head = new QHBoxLayout();
    m_atVal = bigValue(box);
    m_atVal->setObjectName(QStringLiteral("singleChannelAttenValue"));
    m_atCodeLabel = ui::hintLabel(QString(), box);
    head->addWidget(m_atVal);
    head->addWidget(m_atCodeLabel, 1);
    lay->addLayout(head);

    m_atRange = new QSlider(Qt::Horizontal, box);
    lay->addWidget(m_atRange);

    auto* row = new QHBoxLayout();
    auto* dn = new QPushButton(QStringLiteral("−0.5 дБ"), box);
    auto* up = new QPushButton(QStringLiteral("+0.5 дБ"), box);
    m_atDb = new QDoubleSpinBox(box);
    m_atDb->setDecimals(2);
    m_atDb->setSuffix(QStringLiteral(" дБ"));
    m_atNum = new QSpinBox(box);
    m_atNum->setObjectName(QStringLiteral("singleChannelAttenCode"));
    row->addWidget(dn);
    row->addWidget(up);
    row->addWidget(ui::captionLabel(QStringLiteral("дБ"), box));
    row->addWidget(m_atDb);
    row->addWidget(ui::captionLabel(QStringLiteral("код"), box));
    row->addWidget(m_atNum);
    row->addStretch(1);
    lay->addLayout(row);

    connect(m_atRange, &QSlider::valueChanged, this, &SingleChannelPanel::setAttenCode);
    connect(m_atNum, &QSpinBox::valueChanged, this, &SingleChannelPanel::setAttenCode);
    connect(m_atDb, &QDoubleSpinBox::editingFinished, this,
            [this]() { setAttenCode(atten_code_from_db(m_atDb->value(), m_format.atten)); });
    connect(up, &QPushButton::clicked, this, [this]() { setAttenCode(m_atCode + 1); });
    connect(dn, &QPushButton::clicked, this, [this]() { setAttenCode(m_atCode - 1); });
    return box;
}

QWidget* SingleChannelPanel::makeSweepBox()
{
    auto* box = new QGroupBox(QStringLiteral("Перебор состояний"), this);
    auto* grid = new QGridLayout(box);
    ui::applyGroupLayout(grid);

    m_swWhat = new QComboBox(box);
    m_swWhat->addItem(QStringLiteral("фаза"));
    m_swWhat->addItem(QStringLiteral("аттенюация"));
    auto spin = [box](int lo, int hi, int value) {
        auto* s = new QSpinBox(box);
        s->setRange(lo, hi);
        s->setValue(value);
        return s;
    };
    m_swFrom = spin(0, 65535, 0);
    m_swTo = spin(0, 65535, 63);
    m_swStep = spin(1, 65535, 1);
    m_swDwell = spin(50, 600000, 1500);
    m_swDwell->setSuffix(QStringLiteral(" мс"));
    m_swStart = new QPushButton(QStringLiteral("Старт"), box);
    m_swStart->setObjectName(QStringLiteral("singleChannelSweepStart"));
    ui::setButtonRole(m_swStart, ui::ButtonRole::Success);
    m_swStop = new QPushButton(QStringLiteral("Стоп"), box);
    ui::setButtonRole(m_swStop, ui::ButtonRole::Danger);
    m_swState = ui::hintLabel(QString(), box);

    grid->addWidget(ui::captionLabel(QStringLiteral("Что перебирать"), box), 0, 0);
    grid->addWidget(m_swWhat, 0, 1, 1, 3);
    grid->addWidget(ui::captionLabel(QStringLiteral("код от"), box), 1, 0);
    grid->addWidget(m_swFrom, 1, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("до"), box), 1, 2);
    grid->addWidget(m_swTo, 1, 3);
    grid->addWidget(ui::captionLabel(QStringLiteral("шаг кода"), box), 2, 0);
    grid->addWidget(m_swStep, 2, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("пауза"), box), 2, 2);
    grid->addWidget(m_swDwell, 2, 3);
    auto* buttons = new QHBoxLayout();
    buttons->addWidget(m_swStart);
    buttons->addWidget(m_swStop);
    buttons->addWidget(m_swState, 1);
    grid->addLayout(buttons, 3, 0, 1, 4);
    grid->setRowStretch(4, 1);

    connect(m_swStart, &QPushButton::clicked, this, &SingleChannelPanel::startSweep);
    connect(m_swStop, &QPushButton::clicked, this, &SingleChannelPanel::stopSweep);
    return box;
}

QWidget* SingleChannelPanel::makeFormatBox()
{
    auto* box = new QGroupBox(QStringLiteral("Формат кадра"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);

    auto spin = [box](int lo, int hi) {
        auto* s = new QSpinBox(box);
        s->setRange(lo, hi);
        return s;
    };
    auto dspin = [box]() {
        auto* s = new QDoubleSpinBox(box);
        s->setDecimals(4);
        s->setRange(0.0001, 1000.0);
        return s;
    };
    auto combo = [box](std::initializer_list<QString> items) {
        auto* c = new QComboBox(box);
        for (const QString& item : items) {
            c->addItem(item);
        }
        return c;
    };
    auto caption = [box](const QString& text) { return ui::captionLabel(text, box); };

    m_fTotal = spin(1, kMaxFrameBits);
    m_fTotal->setObjectName(QStringLiteral("singleChannelTotalBits"));
    m_fFixed = new QLineEdit(box);
    m_fFixed->setMaxLength(kMaxFrameBits / 4 + 2);
    m_fFixed->setToolTip(QStringLiteral("Биты кадра вне полей фазы и аттенюации, hex"));

    m_fPhOff = spin(0, kMaxFrameBits - 1);
    m_fPhW = spin(1, kMaxFieldBits);
    m_fPhLsb = dspin();
    m_fPhInv = new QCheckBox(QStringLiteral("инверсия"), box);
    m_fPhRev = new QCheckBox(QStringLiteral("обратный порядок бит поля"), box);
    m_fAtOff = spin(0, kMaxFrameBits - 1);
    m_fAtW = spin(1, kMaxFieldBits);
    m_fAtLsb = dspin();
    m_fAtInv = new QCheckBox(QStringLiteral("инверсия"), box);
    m_fAtRev = new QCheckBox(QStringLiteral("обратный порядок бит поля"), box);

    m_cLsb = combo({QStringLiteral("старшим битом вперёд"), QStringLiteral("младшим битом вперёд")});
    m_cClk = combo({QStringLiteral("0 (защёлка по фронту)"), QStringLiteral("1 (защёлка по спаду)")});
    m_cLeAct = combo({QStringLiteral("1"), QStringLiteral("0")});
    m_cLeMode = combo({QStringLiteral("импульс после данных"), QStringLiteral("активен во время передачи")});
    m_cHalf = spin(1, 10000);
    m_cHalf->setSuffix(QStringLiteral(" мкс"));
    m_cLeUs = spin(1, 10000);
    m_cLeUs->setSuffix(QStringLiteral(" мкс"));

    auto* grid = new QGridLayout();
    grid->setHorizontalSpacing(ui::kInnerGap);
    int r = 0;
    grid->addWidget(caption(QStringLiteral("всего бит")), r, 0);
    grid->addWidget(m_fTotal, r, 1);
    grid->addWidget(caption(QStringLiteral("фикс. биты (hex)")), r, 2);
    grid->addWidget(m_fFixed, r, 3, 1, 3);
    ++r;
    auto fieldRow = [&](const QString& title, const char* color, QSpinBox* off, QSpinBox* w,
                        QDoubleSpinBox* lsb, const QString& lsbCaption, QCheckBox* inv, QCheckBox* rev) {
        auto* name = new QLabel(QStringLiteral("<b style=\"color:%1\">%2</b>")
                                    .arg(QLatin1String(color), title),
                                box);
        grid->addWidget(name, r, 0);
        grid->addWidget(caption(QStringLiteral("мл. бит №")), r, 1);
        grid->addWidget(off, r, 2);
        grid->addWidget(caption(QStringLiteral("ширина")), r, 3);
        grid->addWidget(w, r, 4);
        grid->addWidget(caption(lsbCaption), r, 5);
        grid->addWidget(lsb, r, 6);
        grid->addWidget(inv, r, 7);
        grid->addWidget(rev, r, 8);
        ++r;
    };
    fieldRow(QStringLiteral("Фаза:"), kPhaseColor, m_fPhOff, m_fPhW, m_fPhLsb, QStringLiteral("шаг, °"),
             m_fPhInv, m_fPhRev);
    fieldRow(QStringLiteral("Аттенюация:"), kAttenColor, m_fAtOff, m_fAtW, m_fAtLsb,
             QStringLiteral("шаг, дБ"), m_fAtInv, m_fAtRev);
    grid->setColumnStretch(9, 1);
    lay->addLayout(grid);

    auto* lines = new QGridLayout();
    lines->setHorizontalSpacing(ui::kInnerGap);
    lines->addWidget(new QLabel(QStringLiteral("<b>Линии:</b>"), box), 0, 0);
    lines->addWidget(caption(QStringLiteral("порядок")), 0, 1);
    lines->addWidget(m_cLsb, 0, 2);
    lines->addWidget(caption(QStringLiteral("CLK в покое")), 0, 3);
    lines->addWidget(m_cClk, 0, 4);
    lines->addWidget(caption(QStringLiteral("LE активный")), 0, 5);
    lines->addWidget(m_cLeAct, 0, 6);
    lines->addWidget(caption(QStringLiteral("LE")), 1, 1);
    lines->addWidget(m_cLeMode, 1, 2);
    lines->addWidget(caption(QStringLiteral("полупериод CLK")), 1, 3);
    lines->addWidget(m_cHalf, 1, 4);
    lines->addWidget(caption(QStringLiteral("импульс LE")), 1, 5);
    lines->addWidget(m_cLeUs, 1, 6);
    auto* reset = new QPushButton(QStringLiteral("Сбросить по умолчанию"), box);
    ui::setButtonRole(reset, ui::ButtonRole::Secondary);
    lines->addWidget(reset, 1, 7);
    lines->setColumnStretch(8, 1);
    lay->addLayout(lines);

    m_overlap = new QLabel(box);
    m_overlap->setObjectName(QStringLiteral("singleChannelOverlap"));
    m_overlap->setWordWrap(true);
    m_overlap->setProperty("role", QStringLiteral("status"));
    ui::setTone(m_overlap, ui::Tone::Error);
    m_overlap->setVisible(false);
    lay->addWidget(m_overlap);

    m_bits = new QLabel(box);
    m_bits->setObjectName(QStringLiteral("singleChannelBits"));
    m_bits->setTextFormat(Qt::RichText);
    lay->addWidget(m_bits);
    lay->addWidget(new QLabel(QStringLiteral("<span style=\"color:%1\">■ фаза</span>&nbsp;&nbsp;&nbsp;"
                                             "<span style=\"color:%2\">■ аттенюация</span>&nbsp;&nbsp;&nbsp;"
                                             "■ фиксированные биты")
                                  .arg(QLatin1String(kPhaseColor), QLatin1String(kAttenColor)),
                              box));

    for (QSpinBox* s : {m_fTotal, m_fPhOff, m_fPhW, m_fAtOff, m_fAtW, m_cHalf, m_cLeUs}) {
        connect(s, &QSpinBox::editingFinished, this, &SingleChannelPanel::onFormatEdited);
    }
    for (QDoubleSpinBox* s : {m_fPhLsb, m_fAtLsb}) {
        connect(s, &QDoubleSpinBox::editingFinished, this, &SingleChannelPanel::onFormatEdited);
    }
    connect(m_fFixed, &QLineEdit::editingFinished, this, &SingleChannelPanel::onFormatEdited);
    for (QCheckBox* c : {m_fPhInv, m_fPhRev, m_fAtInv, m_fAtRev}) {
        connect(c, &QCheckBox::toggled, this, &SingleChannelPanel::onFormatEdited);
    }
    for (QComboBox* c : {m_cLsb, m_cClk, m_cLeAct, m_cLeMode}) {
        connect(c, &QComboBox::currentIndexChanged, this, &SingleChannelPanel::onFormatEdited);
    }
    connect(reset, &QPushButton::clicked, this, &SingleChannelPanel::resetFormat);
    return box;
}

QWidget* SingleChannelPanel::makeLogBox()
{
    auto* box = new QGroupBox(QStringLiteral("Обмен с платой"), this);
    auto* lay = new QVBoxLayout(box);
    ui::applyGroupLayout(lay);
    m_log = new QPlainTextEdit(box);
    m_log->setObjectName(QStringLiteral("singleChannelLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setFixedHeight(120);
    lay->addWidget(m_log);
    return box;
}

// --- Подключение ---

void SingleChannelPanel::refreshPorts()
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
        m_port->setCurrentIndex(0);  // плата ППКХ4, если найдена, стоит первой
    } else {
        m_port->setEditText(current);
    }
}

void SingleChannelPanel::toggleConnection()
{
    if (m_connected) {
        stopSweep();
        m_link->close();
        onClosed(QString());
        return;
    }
    // Подпись списка — «имя — описание»; вручную можно вписать одно имя.
    const QString name = m_port->currentText().section(QLatin1Char(' '), 0, 0);
    if (!m_link->open(name)) {
        setConnState(QStringLiteral("порт не открыт: ") + m_link->lastError(), false, true);
    }
}

void SingleChannelPanel::onOpened()
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
        QSettings().setValue(settingsKey("port"), m_link->portName());
        updateButtons();
        applyCfg();
        sendFrame();
    });
    updateButtons();
}

void SingleChannelPanel::onClosed(const QString& reason)
{
    stopSweep();
    m_connected = false;
    m_lastSent->clear();
    if (reason.isEmpty()) {
        setConnState(QStringLiteral("не подключено"), false, false);
    } else {
        setConnState(QStringLiteral("связь потеряна: ") + reason, false, true);
    }
    updateButtons();
}

void SingleChannelPanel::setConnState(const QString& text, bool ok, bool bad)
{
    m_conn->setText(text);
    ui::setTone(m_conn, ok ? ui::Tone::Ok : (bad ? ui::Tone::Error : ui::Tone::Neutral));
}

void SingleChannelPanel::updateButtons()
{
    const bool open = m_link->isOpen();
    m_connect->setText(open ? QStringLiteral("Отключить") : QStringLiteral("Подключить"));
    ui::setButtonRole(m_connect, open ? ui::ButtonRole::Danger : ui::ButtonRole::Primary);
    m_connect->setEnabled(Ppkh4Link::serialSupported());
    m_port->setEnabled(!open);
    m_refresh->setEnabled(!open);
    m_swStart->setEnabled(m_connected && !m_sweeping);
    m_swStop->setEnabled(m_sweeping);
    if (m_quickPhRange) {
        m_quickPhRange->parentWidget()->setEnabled(!m_sweeping);
    }
}

// --- Настройки ---

void SingleChannelPanel::loadSettings()
{
    QSettings s;
    const FrameFormat d;
    const LineConfig l;
    m_format.total_bits = s.value(settingsKey("totalBits"), d.total_bits).toInt();
    m_format.fixed_hex = s.value(settingsKey("fixedHex"), QString::fromStdString(d.fixed_hex)).toString().toStdString();
    m_format.phase.offset = s.value(settingsKey("phOff"), d.phase.offset).toInt();
    m_format.phase.width = s.value(settingsKey("phW"), d.phase.width).toInt();
    m_format.phase.lsb = s.value(settingsKey("phLsb"), d.phase.lsb).toDouble();
    m_format.phase.invert = s.value(settingsKey("phInv"), d.phase.invert).toBool();
    m_format.phase.reverse = s.value(settingsKey("phRev"), d.phase.reverse).toBool();
    m_format.atten.offset = s.value(settingsKey("atOff"), d.atten.offset).toInt();
    m_format.atten.width = s.value(settingsKey("atW"), d.atten.width).toInt();
    m_format.atten.lsb = s.value(settingsKey("atLsb"), d.atten.lsb).toDouble();
    m_format.atten.invert = s.value(settingsKey("atInv"), d.atten.invert).toBool();
    m_format.atten.reverse = s.value(settingsKey("atRev"), d.atten.reverse).toBool();
    m_lines.lsb_first = s.value(settingsKey("lsbFirst"), l.lsb_first).toInt();
    m_lines.clk_idle = s.value(settingsKey("clkIdle"), l.clk_idle).toInt();
    m_lines.le_active = s.value(settingsKey("leActive"), l.le_active).toInt();
    m_lines.le_mode = s.value(settingsKey("leMode"), l.le_mode).toInt();
    m_lines.half_us = s.value(settingsKey("halfUs"), l.half_us).toInt();
    m_lines.le_us = s.value(settingsKey("leUs"), l.le_us).toInt();
    if (!format_valid(m_format)) {
        m_format = d;
    }
    m_port->setEditText(s.value(settingsKey("port")).toString());
}

void SingleChannelPanel::saveSettings() const
{
    QSettings s;
    s.setValue(settingsKey("totalBits"), m_format.total_bits);
    s.setValue(settingsKey("fixedHex"), QString::fromStdString(m_format.fixed_hex));
    s.setValue(settingsKey("phOff"), m_format.phase.offset);
    s.setValue(settingsKey("phW"), m_format.phase.width);
    s.setValue(settingsKey("phLsb"), m_format.phase.lsb);
    s.setValue(settingsKey("phInv"), m_format.phase.invert);
    s.setValue(settingsKey("phRev"), m_format.phase.reverse);
    s.setValue(settingsKey("atOff"), m_format.atten.offset);
    s.setValue(settingsKey("atW"), m_format.atten.width);
    s.setValue(settingsKey("atLsb"), m_format.atten.lsb);
    s.setValue(settingsKey("atInv"), m_format.atten.invert);
    s.setValue(settingsKey("atRev"), m_format.atten.reverse);
    s.setValue(settingsKey("lsbFirst"), m_lines.lsb_first);
    s.setValue(settingsKey("clkIdle"), m_lines.clk_idle);
    s.setValue(settingsKey("leActive"), m_lines.le_active);
    s.setValue(settingsKey("leMode"), m_lines.le_mode);
    s.setValue(settingsKey("halfUs"), m_lines.half_us);
    s.setValue(settingsKey("leUs"), m_lines.le_us);
}

void SingleChannelPanel::loadForm()
{
    m_loading = true;
    m_fTotal->setValue(m_format.total_bits);
    m_fFixed->setText(QString::fromStdString(m_format.fixed_hex));
    m_fPhOff->setValue(m_format.phase.offset);
    m_fPhW->setValue(m_format.phase.width);
    m_fPhLsb->setValue(m_format.phase.lsb);
    m_fPhInv->setChecked(m_format.phase.invert);
    m_fPhRev->setChecked(m_format.phase.reverse);
    m_fAtOff->setValue(m_format.atten.offset);
    m_fAtW->setValue(m_format.atten.width);
    m_fAtLsb->setValue(m_format.atten.lsb);
    m_fAtInv->setChecked(m_format.atten.invert);
    m_fAtRev->setChecked(m_format.atten.reverse);
    m_cLsb->setCurrentIndex(m_lines.lsb_first != 0 ? 1 : 0);
    m_cClk->setCurrentIndex(m_lines.clk_idle != 0 ? 1 : 0);
    m_cLeAct->setCurrentIndex(m_lines.le_active != 0 ? 0 : 1);
    m_cLeMode->setCurrentIndex(m_lines.le_mode != 0 ? 1 : 0);
    m_cHalf->setValue(m_lines.half_us);
    m_cLeUs->setValue(m_lines.le_us);
    m_loading = false;
}

void SingleChannelPanel::readForm()
{
    m_format.total_bits = m_fTotal->value();
    m_format.fixed_hex = m_fFixed->text().trimmed().toStdString();
    m_format.phase = {m_fPhOff->value(), m_fPhW->value(), m_fPhLsb->value(), m_fPhInv->isChecked(),
                      m_fPhRev->isChecked()};
    m_format.atten = {m_fAtOff->value(), m_fAtW->value(), m_fAtLsb->value(), m_fAtInv->isChecked(),
                      m_fAtRev->isChecked()};
    m_lines.lsb_first = m_cLsb->currentIndex();
    m_lines.clk_idle = m_cClk->currentIndex();
    m_lines.le_active = m_cLeAct->currentIndex() == 0 ? 1 : 0;
    m_lines.le_mode = m_cLeMode->currentIndex();
    m_lines.half_us = m_cHalf->value();
    m_lines.le_us = m_cLeUs->value();
}

void SingleChannelPanel::onFormatEdited()
{
    if (m_loading) {
        return;
    }
    readForm();
    saveSettings();
    m_phCode = clamp_code(m_phCode, m_format.phase);
    m_atCode = clamp_code(m_atCode, m_format.atten);
    render();
    if (m_connected) {
        applyCfg();
        sendFrame();
    }
}

void SingleChannelPanel::resetFormat()
{
    m_format = FrameFormat{};
    m_lines = LineConfig{};
    saveSettings();
    loadForm();
    onFormatEdited();
}

// --- Отображение и запись ---

void SingleChannelPanel::render()
{
    const int phMax = max_code(m_format.phase.width);
    const int atMax = max_code(m_format.atten.width);
    const double deg = phase_deg(m_phCode, m_format.phase);
    const double db = atten_db(m_atCode, m_format.atten);
    {
        const QSignalBlocker b1(m_phRange), b2(m_phNum), b3(m_atRange), b4(m_atNum);
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
    }
    m_atDb->setRange(-atMax * m_format.atten.lsb, 0.0);
    if (!m_phDeg->hasFocus()) {
        m_phDeg->setValue(deg);
    }
    if (!m_atDb->hasFocus()) {
        m_atDb->setValue(db);
    }
    m_phVal->setText(QString::number(deg, 'f', 3) + QStringLiteral("°"));
    m_atVal->setText(QString::number(db, 'f', 2) + QStringLiteral(" дБ"));
    m_phCodeLabel->setText(QStringLiteral("код %1 / %2").arg(m_phCode).arg(binary(m_phCode, m_format.phase.width)));
    m_atCodeLabel->setText(QStringLiteral("код %1 / %2").arg(m_atCode).arg(binary(m_atCode, m_format.atten.width)));
    if (m_quickPhRange) {
        const QSignalBlocker b1(m_quickPhRange), b2(m_quickPhDeg);
        const QSignalBlocker b3(m_quickAtRange), b4(m_quickAtDb);
        m_quickPhRange->setRange(0, phMax);
        m_quickAtRange->setRange(0, atMax);
        m_quickPhRange->setValue(m_phCode);
        m_quickAtRange->setValue(m_atCode);
        m_quickPhDeg->setRange(0.0, phase_deg(phMax, m_format.phase));
        m_quickPhDeg->setSingleStep(m_format.phase.lsb);
        m_quickAtDb->setRange(atten_db(0, m_format.atten), 0.0);
        m_quickAtDb->setSingleStep(m_format.atten.lsb);
        if (!m_quickPhDeg->hasFocus()) m_quickPhDeg->setValue(deg);
        if (!m_quickAtDb->hasFocus()) m_quickAtDb->setValue(db);
    }

    const bool overlap = fields_overlap(m_format);
    const bool outside = fields_outside(m_format);
    m_overlap->setVisible(overlap || outside);
    m_overlap->setText(overlap ? QStringLiteral("Поля фазы и аттенюации перекрываются: часть бит фазы "
                                                "затирается аттенюацией. Проверьте «мл. бит №».")
                               : QStringLiteral("Поле выходит за пределы кадра: увеличьте «всего бит» "
                                                "или сдвиньте поле."));

    // Биты кадра старшим слева, под каждым — номер бита.
    const auto bits = build_frame(m_format, m_phCode, m_atCode);
    QString top;
    QString bottom;
    const auto& ph = m_format.phase;
    const auto& at = m_format.atten;
    for (int i = static_cast<int>(bits.size()) - 1; i >= 0; --i) {
        QString color;
        if (i >= ph.offset && i < ph.offset + ph.width) {
            color = QLatin1String(kPhaseColor);
        } else if (i >= at.offset && i < at.offset + at.width) {
            color = QLatin1String(kAttenColor);
        }
        const bool one = bits[static_cast<std::size_t>(i)];
        const QString style = color.isEmpty() ? QString() : QStringLiteral(" style=\"color:%1\"").arg(color);
        top += QStringLiteral("<td align=\"center\"%1>%2</td>")
                   .arg(style, one ? QStringLiteral("<b>1</b>") : QStringLiteral("0"));
        bottom += QStringLiteral("<td align=\"center\"><small>%1</small></td>").arg(i);
    }
    m_bits->setText(QStringLiteral("<table cellspacing=\"2\" cellpadding=\"3\" style=\"font-family:monospace\">"
                                   "<tr>%1</tr><tr>%2</tr></table>")
                        .arg(top, bottom));
}

void SingleChannelPanel::changed()
{
    if (m_connected) {
        m_sendDebounce->start();
    }
}

void SingleChannelPanel::applyCfg()
{
    for (const auto& cmd : cfg_commands(m_lines)) {
        m_link->send(QString::fromStdString(cmd));
    }
}

void SingleChannelPanel::sendFrame(const std::function<void()>& after)
{
    m_sendDebounce->stop();
    const auto bits = build_frame(m_format, m_phCode, m_atCode);
    const QString hex = QString::fromStdString(frame_hex(bits));
    const double deg = phase_deg(m_phCode, m_format.phase);
    const double db = atten_db(m_atCode, m_format.atten);
    m_link->send(sendCommand(), [this, hex, deg, db, after](const QString& reply) {
        if (reply.startsWith(QLatin1String("OK"))) {
            m_lastSent->setText(QStringLiteral("записано: %1°, %2 дБ (0x%3)")
                                    .arg(QString::number(deg, 'f', 3), QString::number(db, 'f', 2), hex));
        }
        if (after) {
            after();
        }
    });
}

// --- Перебор ---

void SingleChannelPanel::startSweep()
{
    if (!m_connected || m_sweeping) {
        return;
    }
    m_sweeping = true;
    m_sweepCode = m_swFrom->value();
    updateButtons();
    m_sweepTimer->start(0);
}

void SingleChannelPanel::stopSweep()
{
    m_sweepTimer->stop();
    m_sweeping = false;
    m_swState->clear();
    updateButtons();
}

void SingleChannelPanel::sweepTick()
{
    if (!m_sweeping) {
        return;
    }
    const bool phase = m_swWhat->currentIndex() == 0;
    const int from = m_swFrom->value();
    const int to = m_swTo->value();
    const int step = std::max(1, m_swStep->value());
    if (phase) {
        m_phCode = std::min(m_sweepCode, max_code(m_format.phase.width));
    } else {
        m_atCode = std::min(m_sweepCode, max_code(m_format.atten.width));
    }
    render();
    m_swState->setText(phase ? QStringLiteral("фаза %1°").arg(QString::number(phase_deg(m_phCode, m_format.phase), 'f', 3))
                             : QStringLiteral("атт. %1 дБ").arg(QString::number(atten_db(m_atCode, m_format.atten), 'f', 2)));
    m_sweepCode += from <= to ? step : -step;
    if ((from <= to && m_sweepCode > to) || (from > to && m_sweepCode < to)) {
        m_sweepCode = from;
    }
    sendFrame([this]() {
        if (m_sweeping) {
            m_sweepTimer->start(std::max(50, m_swDwell->value()));
        }
    });
}

void SingleChannelPanel::appendLog(const QString& line)
{
    m_log->appendPlainText(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz  ")) + line);
}
