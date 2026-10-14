#include "FullScanPanel.h"

#include "PsSession.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

constexpr double kMhz = 1e6;
constexpr double kGhz = 1e9;

/// Значение и единицы рядом: «МГц» / «ГГц». Смена единиц пересчитывает число.
void makeFrequency(QWidget* parent, double hz, int unitIndex, QDoubleSpinBox*& value, QComboBox*& unit)
{
    value = new QDoubleSpinBox(parent);
    value->setButtonSymbols(QAbstractSpinBox::NoButtons);
    value->setDecimals(3);
    value->setRange(0.0, double(ps::kFrequencyLimitHz) / kMhz);
    unit = new QComboBox(parent);
    unit->addItem(QStringLiteral("МГц"), kMhz);
    unit->addItem(QStringLiteral("ГГц"), kGhz);
    unit->setCurrentIndex(unitIndex);
    value->setValue(hz / unit->currentData().toDouble());
    QObject::connect(unit, &QComboBox::currentIndexChanged, value,
                     [value, unit, scale = unit->currentData().toDouble()](int) mutable {
                         const double next = unit->currentData().toDouble();
                         const double hzNow = value->value() * scale;
                         value->setValue(hzNow / next);
                         scale = next;
                     });
}

double toHz(const QDoubleSpinBox* value, const QComboBox* unit)
{
    return value->value() * unit->currentData().toDouble();
}

QDoubleSpinBox* degBox(QWidget* parent, double value)
{
    auto* box = new QDoubleSpinBox(parent);
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    box->setDecimals(1);
    box->setRange(-ps::kAzimuthLimitDeg, ps::kAzimuthLimitDeg);
    box->setSuffix(QStringLiteral("°"));
    box->setValue(value);
    return box;
}

}  // namespace

FullScanPanel::FullScanPanel(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kInnerGap);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(ui::kInnerGap);
    grid->setVerticalSpacing(ui::kInnerGap);
    makeFrequency(this, 4.9e9, 0, m_freqFrom, m_freqFromUnit);
    makeFrequency(this, 6.0e9, 1, m_freqTo, m_freqToUnit);
    m_freqFrom->setObjectName(QStringLiteral("sffFreqFrom"));
    m_freqTo->setObjectName(QStringLiteral("sffFreqTo"));
    m_freqFromUnit->setObjectName(QStringLiteral("sffFreqFromUnit"));
    m_freqToUnit->setObjectName(QStringLiteral("sffFreqToUnit"));
    m_azFrom = degBox(this, -ps::kAzimuthLimitDeg);
    m_azFrom->setObjectName(QStringLiteral("sffAzFrom"));
    m_azTo = degBox(this, ps::kAzimuthLimitDeg);
    m_azTo->setObjectName(QStringLiteral("sffAzTo"));
    m_period = new QSpinBox(this);
    m_period->setObjectName(QStringLiteral("sffPeriod"));
    m_period->setRange(0, ps::kSffPeriodMaxSec);
    m_period->setSuffix(QStringLiteral(" с"));
    m_period->setSpecialValueText(QStringLiteral("однократно"));
    m_period->setButtonSymbols(QAbstractSpinBox::NoButtons);

    grid->addWidget(new QLabel(QStringLiteral("Частоты"), this), 0, 0);
    grid->addWidget(m_freqFrom, 0, 1);
    grid->addWidget(m_freqFromUnit, 0, 2);
    grid->addWidget(m_freqTo, 0, 3);
    grid->addWidget(m_freqToUnit, 0, 4);
    grid->addWidget(new QLabel(QStringLiteral("Сектор"), this), 1, 0);
    grid->addWidget(m_azFrom, 1, 1);
    grid->addWidget(m_azTo, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("Период обзора"), this), 2, 0);
    grid->addWidget(m_period, 2, 1, 1, 2);
    grid->setColumnStretch(5, 1);
    layout->addLayout(grid);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(ui::kInnerGap);
    m_apply = new QPushButton(QStringLiteral("Применить параметры"), this);
    m_apply->setObjectName(QStringLiteral("sffApply"));
    m_start = new QPushButton(QStringLiteral("Старт"), this);
    m_start->setObjectName(QStringLiteral("sffStart"));
    m_stop = new QPushButton(QStringLiteral("Стоп"), this);
    m_stop->setObjectName(QStringLiteral("sffStop"));
    ui::setButtonRole(m_apply, ui::ButtonRole::Secondary);
    ui::setButtonRole(m_start, ui::ButtonRole::Success);
    ui::setButtonRole(m_stop, ui::ButtonRole::Danger);
    buttons->addWidget(m_apply);
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    m_state = new QLabel(this);
    m_state->setObjectName(QStringLiteral("sffState"));
    m_state->setProperty("role", QStringLiteral("hint"));
    m_state->setWordWrap(true);
    layout->addWidget(m_state);

    layout->addWidget(ui::captionLabel(QStringLiteral("Результаты"), this));
    m_results = new QTableWidget(0, 4, this);
    m_results->setObjectName(QStringLiteral("sffResults"));
    m_results->setHorizontalHeaderLabels({QStringLiteral("Частота, МГц"), QStringLiteral("Калибровка"),
                                          QStringLiteral("Азимут максимума"), QStringLiteral("Мощность, макс.")});
    m_results->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_results->verticalHeader()->setVisible(false);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setAlternatingRowColors(true);
    m_results->setShowGrid(false);
    m_results->setMinimumHeight(140);
    layout->addWidget(m_results, 1);

    connect(m_apply, &QPushButton::clicked, this, [this] { inScanMode([this] { apply(); }); });
    connect(m_start, &QPushButton::clicked, this, [this] { inScanMode([this] { start(); }); });
    connect(m_stop, &QPushButton::clicked, this, &FullScanPanel::stop);
    connect(m_session, &PsSession::controlsChanged, this, &FullScanPanel::refreshControls);
    connect(m_session, &PsSession::modeChanged, this, [this] {
        refreshControls();
        if (m_afterModeChange && m_session->modeKnown() && m_session->mode() == ps::Mode::FullScanFreq) {
            auto action = std::move(m_afterModeChange);
            m_afterModeChange = nullptr;
            action();
        }
    });
    connect(m_session, &PsSession::sffChanged, this, [this] {
        refreshParams();
        refreshResults();
    });
    refreshControls();
    refreshResults();
}

bool FullScanPanel::readParams(ps::SffParams& out, QString* error) const
{
    ps::SffParams p;
    p.freq_min = std::llround(toHz(m_freqFrom, m_freqFromUnit));
    p.freq_max = std::llround(toHz(m_freqTo, m_freqToUnit));
    p.az_min = m_azFrom->value();
    p.az_max = m_azTo->value();
    p.period = m_period->value();
    std::string why;
    if (!p.validate(&why)) {
        if (error) {
            *error = QString::fromStdString(why);
        }
        return false;
    }
    out = p;
    return true;
}

void FullScanPanel::inScanMode(std::function<void()> action)
{
    if (!m_session->manualAllowed()) {
        return;
    }
    if (m_session->modeKnown() && m_session->mode() == ps::Mode::FullScanFreq) {
        action();
        return;
    }
    m_afterModeChange = std::move(action);
    m_state->setText(QStringLiteral("Перевожу изделие в режим «Полное сканирование»…"));
    if (m_session->send(ps::msg::SetMode, json{{"mode", "FULL_SCAN_FREQ"}}) < 0) {
        m_afterModeChange = nullptr;
        m_state->setText(m_session->lastError());
    }
}

void FullScanPanel::apply()
{
    ps::SffParams p;
    QString error;
    if (!readParams(p, &error)) {
        m_state->setText(QStringLiteral("Не отправлено: %1").arg(error));
        return;
    }
    if (m_session->send(ps::msg::SetSffParameters, p.toJson()) >= 0) {
        m_state->setText(QStringLiteral("Параметры обзора отправлены"));
    }
}

void FullScanPanel::start()
{
    // Параметры с формы уходят перед стартом: обзор идёт ровно по тому, что видно.
    ps::SffParams p;
    QString error;
    if (!readParams(p, &error)) {
        m_state->setText(QStringLiteral("Не отправлено: %1").arg(error));
        return;
    }
    if (m_session->send(ps::msg::SetSffParameters, p.toJson()) < 0) {
        return;
    }
    if (m_session->send(ps::msg::SetSffStart) >= 0) {
        m_state->setText(QStringLiteral("Обзор запущен, результаты приходят по одной частоте"));
    }
}

void FullScanPanel::stop()
{
    m_afterModeChange = nullptr;
    if (m_session->send(ps::msg::SetSffStop) >= 0) {
        m_state->setText(QStringLiteral("Стоп: текущий обзор доводится до конца"));
    }
}

void FullScanPanel::refreshControls()
{
    const bool allowed = m_session->manualAllowed();
    m_apply->setEnabled(allowed);
    m_start->setEnabled(allowed);
    m_stop->setEnabled(allowed && m_session->modeKnown() && m_session->mode() == ps::Mode::FullScanFreq);
    if (!m_session->link()->isReady()) {
        m_state->setText(QStringLiteral("Нет связи с PS — подключитесь слева"));
    } else if (m_session->modeKnown() && !m_afterModeChange) {
        m_state->setText(QStringLiteral("Режим изделия: %1%2")
                             .arg(QString::fromStdString(ps::modeRu(m_session->mode())),
                                  m_session->state().isEmpty()
                                      ? QString()
                                      : QStringLiteral(", %1").arg(QString::fromStdString(
                                            ps::stateRu(m_session->state().toStdString())))));
    }
}

void FullScanPanel::refreshParams()
{
    const auto& p = m_session->sffParams();
    if (!p) {
        return;
    }
    if (!m_freqFrom->hasFocus()) {
        m_freqFrom->setValue(double(p->freq_min) / m_freqFromUnit->currentData().toDouble());
    }
    if (!m_freqTo->hasFocus()) {
        m_freqTo->setValue(double(p->freq_max) / m_freqToUnit->currentData().toDouble());
    }
    if (!m_azFrom->hasFocus()) {
        m_azFrom->setValue(p->az_min);
    }
    if (!m_azTo->hasFocus()) {
        m_azTo->setValue(p->az_max);
    }
    if (!m_period->hasFocus()) {
        m_period->setValue(p->period);
    }
}

void FullScanPanel::refreshResults()
{
    const auto& results = m_session->sffResults();
    m_results->setRowCount(static_cast<int>(results.size()));
    int row = 0;
    for (const auto& [freq, r] : results) {
        double power = 0.0;
        double azimuth = 0.0;
        const bool any = r.peak(power, azimuth);
        const QStringList cells{
            QString::number(double(freq) / kMhz, 'f', 3),
            r.cal ? QStringLiteral("есть") : QStringLiteral("нет"),
            any ? QStringLiteral("%1°").arg(azimuth, 0, 'f', 1) : QStringLiteral("—"),
            any ? QString::number(power, 'f', 1) : QStringLiteral("—"),
        };
        for (int c = 0; c < cells.size(); ++c) {
            m_results->setItem(row, c, new QTableWidgetItem(cells.at(c)));
        }
        ++row;
    }
}
