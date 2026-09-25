#include "MeasureTab.h"

#include "S21PlotWidget.h"

#include "../cal/PhaseMath.h"
#include "../measure/RunStateMachine.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {

const std::array<QString, 6> kGraphNames = {
    QStringLiteral("S11"), QStringLiteral("S21"),
    QStringLiteral("S12"), QStringLiteral("S22"),
    QStringLiteral("КСВН-1"), QStringLiteral("КСВН-2")};
const std::array<QColor, 6> kGraphColors = {
    QColor(0x2F, 0x80, 0xED), QColor(0x27, 0xAE, 0x60),
    QColor(0xF2, 0x99, 0x4A), QColor(0xBB, 0x6B, 0xD9),
    QColor(0xD6, 0x27, 0x28), QColor(0x17, 0xBE, 0xCF)};

QVector<double> vswrFromReturnLossDb(const QVector<double>& reflectionDb)
{
    QVector<double> result;
    result.reserve(reflectionDb.size());
    for (double db : reflectionDb) {
        result.push_back(afar::cal::vswr_from_reflection_db(db));
    }
    return result;
}

qsizetype nearestFrequencyIndex(const QVector<double>& frequency, double value)
{
    if (frequency.isEmpty()) {
        return -1;
    }
    const auto it = std::lower_bound(frequency.cbegin(), frequency.cend(), value);
    if (it == frequency.cbegin()) {
        return 0;
    }
    if (it == frequency.cend()) {
        return frequency.size() - 1;
    }
    const qsizetype right = std::distance(frequency.cbegin(), it);
    const qsizetype left = right - 1;
    return std::abs(frequency[left] - value) <= std::abs(frequency[right] - value)
        ? left : right;
}

} // namespace

MeasureTab::MeasureTab(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_stages = new QListWidget(this);
    const QStringList stages = {
        QStringLiteral("1 Подключения"),
        QStringLiteral("2 Калибровка ВАЦ (OSL/SOLT)"),
        QStringLiteral("3 Линейность"),
        QStringLiteral("4 Перебор кодов"),
        QStringLiteral("5 Прямая LUT"),
        QStringLiteral("6 Обратная LUT"),
        QStringLiteral("7 Валидация"),
        QStringLiteral("8 Графики"),
    };
    m_stages->addItems(stages);
    m_stages->setCurrentRow(0);
    m_stages->setFixedWidth(220);
    connect(m_stages, &QListWidget::currentRowChanged, this, &MeasureTab::onStageClicked);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(makeConnectionsPage());
    m_stack->addWidget(makeCalPage());
    m_stack->addWidget(makeLinearityPage());
    m_stack->addWidget(makeSweepPage());
    m_stack->addWidget(makeDirectPage());
    m_stack->addWidget(makeInversePage());
    m_stack->addWidget(makeValidationPage());
    m_stack->addWidget(makeGraphsPage());

    // График — стартовая рабочая страница вкладки «Измерение».
    m_stages->setCurrentRow(7);
    m_stack->setCurrentIndex(7);

    root->addWidget(m_stages);
    root->addWidget(m_stack, 1);

    // UI-303 / пресет @1296: 1246…1346 МГц, 101 точка, IFBW 1 кГц.
    applyRunConfigDefaults(1.246e9, 1.346e9, 101, 1000, -30.0, 8);
    setStandCheck(false, false, false, QStringLiteral("не измерено"), false, false, -30.0, false);
    setSeriesArtifacts(QStringLiteral("—"), QStringLiteral("direct-lut.parquet"),
                       QStringLiteral("inverse-lut.parquet"), QStringLiteral("report.pdf"),
                       QStringLiteral("manifest.sha256"));
    setRunStateGuide(static_cast<int>(afar::RunState::Idle));
}

QWidget* MeasureTab::makeConnectionsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(QStringLiteral("Этап 1. Подключения и мастер запуска"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));

    m_connectHow = new QLabel(
        QStringLiteral(
            "Как запустить серию:\n"
            "1. «Мастер запуска» — чеклист подключений и калибровки ВАЦ в S2VNA.\n"
            "2. На каждом шаге — «Подтверждаю», затем «Далее» / «Готово».\n"
            "3. Зелёная «Старт» — перебор кодов. Смотрите бейдж «Источник: …» "
            "(имитатор ≠ метрология стенда).\n"
            "Частоты и точки свипа — на этапе «4 Перебор кодов»."),
        page);
    m_connectHow->setWordWrap(true);
    m_connectHow->setObjectName(QStringLiteral("hintLabel"));

    auto* openWizard = new QPushButton(QStringLiteral("Открыть мастер запуска"), page);
    openWizard->setObjectName(QStringLiteral("btnPrimary"));
    connect(openWizard, &QPushButton::clicked, this, &MeasureTab::openWizardRequested);

    auto* resume = new QPushButton(QStringLiteral("Продолжить незакрытую серию"), page);
    resume->setObjectName(QStringLiteral("btnResume"));
    connect(resume, &QPushButton::clicked, this, &MeasureTab::resumeSeriesRequested);

    m_unfinished = new QLabel(QStringLiteral("Незакрытая серия не найдена."), page);
    m_unfinished->setWordWrap(true);
    m_unfinished->setObjectName(QStringLiteral("hintLabel"));

    layout->addWidget(title);
    layout->addWidget(m_connectHow);
    layout->addWidget(openWizard);
    layout->addWidget(resume);
    layout->addWidget(m_unfinished);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeCalPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 2. Калибровка ВАЦ из AFAR"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));

    auto* transportNote = new QLabel(
        QStringLiteral(
            "SCPI через текущий VNA-транспорт (Socket/COM); отдельный UI S2VNA для "
            "калибровки не требуется. На имитаторе шаги проходят без стендовой "
            "метрологии. Перед каждым шагом — подтверждение оператора."),
        page);
    transportNote->setWordWrap(true);
    transportNote->setObjectName(QStringLiteral("hintLabel"));

    auto* kindRow = new QHBoxLayout();
    auto* kindLabel = new QLabel(QStringLiteral("Тип калибровки:"), page);
    m_calKind = new QComboBox(page);
    m_calKind->addItem(QStringLiteral("1-портовая (OSL)"), 0);
    m_calKind->addItem(QStringLiteral("2-портовая (SOLT)"), 1);
    m_calKind->setCurrentIndex(1); // прежнее поведение по умолчанию
    connect(m_calKind, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onCalKindChanged);
    kindRow->addWidget(kindLabel);
    kindRow->addWidget(m_calKind, 1);

    m_calKindHint = new QLabel(
        QStringLiteral(
            "1-порт — для S11/S22-трактов; 2-порт — для полного S-параметрического комплекта."),
        page);
    m_calKindHint->setWordWrap(true);
    m_calKindHint->setObjectName(QStringLiteral("hintLabel"));

    auto* portRow = new QHBoxLayout();
    m_calPortLabel = new QLabel(QStringLiteral("Порт ВАЦ:"), page);
    m_calPort = new QComboBox(page);
    m_calPort->addItem(QStringLiteral("Порт 1"), 1);
    m_calPort->addItem(QStringLiteral("Порт 2"), 2);
    portRow->addWidget(m_calPortLabel);
    portRow->addWidget(m_calPort, 1);

    m_soltStepLabel = new QLabel(page);
    m_soltStepLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_soltHint = new QLabel(page);
    m_soltHint->setWordWrap(true);
    m_soltHint->setObjectName(QStringLiteral("hintLabel"));

    auto* btnRow = new QHBoxLayout();
    m_soltConfirm = new QPushButton(QStringLiteral("Подтверждаю — выполнить шаг"), page);
    m_soltConfirm->setObjectName(QStringLiteral("btnPrimary"));
    m_soltReset = new QPushButton(QStringLiteral("Сначала"), page);
    connect(m_soltConfirm, &QPushButton::clicked, this, &MeasureTab::onSoltConfirmClicked);
    connect(m_soltReset, &QPushButton::clicked, this, &MeasureTab::onSoltResetClicked);
    btnRow->addWidget(m_soltConfirm);
    btnRow->addWidget(m_soltReset);
    btnRow->addStretch(1);

    m_calExtraGroup = new QGroupBox(QStringLiteral("Доп. чеклист Response / Thru (опционально)"), page);
    auto* extraLay = new QVBoxLayout(m_calExtraGroup);
    m_calStepResponse = new QCheckBox(QStringLiteral("Response (нормализация) выполнен"), m_calExtraGroup);
    m_calStepThru = new QCheckBox(QStringLiteral("Thru выполнен"), m_calExtraGroup);
    extraLay->addWidget(m_calStepResponse);
    extraLay->addWidget(m_calStepThru);

    m_calText = new QLabel(page);
    m_calText->setWordWrap(true);
    m_calText->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(title);
    layout->addWidget(transportNote);
    layout->addLayout(kindRow);
    layout->addWidget(m_calKindHint);
    layout->addLayout(portRow);
    layout->addWidget(m_soltStepLabel);
    layout->addWidget(m_soltHint);
    layout->addLayout(btnRow);
    layout->addWidget(m_calExtraGroup);
    layout->addWidget(m_calText);
    layout->addStretch(1);
    onCalKindChanged(m_calKind->currentIndex());
    return page;
}

QWidget* MeasureTab::makeLinearityPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 3. Линейность и перегрузка"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_linText = new QLabel(page);
    m_linText->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(m_linText);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeSweepPage()
{
    auto* page = new QWidget(this);
    auto* center = new QVBoxLayout(page);

    auto* params = new QGroupBox(QStringLiteral("Параметры свипа"), page);
    auto* form = new QFormLayout(params);

    auto makeFreqRow = [params](QDoubleSpinBox** spinOut, QComboBox** unitOut) {
        auto* row = new QWidget(params);
        auto* lay = new QHBoxLayout(row);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* spin = new QDoubleSpinBox(row);
        spin->setDecimals(3);
        auto* unit = new QComboBox(row);
        unit->addItem(QStringLiteral("Гц"));
        unit->addItem(QStringLiteral("кГц"));
        unit->addItem(QStringLiteral("МГц"));
        unit->addItem(QStringLiteral("ГГц"));
        unit->setCurrentIndex(2); // МГц
        lay->addWidget(spin, 1);
        lay->addWidget(unit);
        *spinOut = spin;
        *unitOut = unit;
        return row;
    };

    auto* startRow = makeFreqRow(&m_fStart, &m_fStartUnit);
    auto* stopRow = makeFreqRow(&m_fStop, &m_fStopUnit);

    m_points = new QSpinBox(params);
    m_points->setRange(2, 10001);

    auto* ifbwRow = new QWidget(params);
    {
        auto* lay = new QHBoxLayout(ifbwRow);
        lay->setContentsMargins(0, 0, 0, 0);
        m_ifbw = new QDoubleSpinBox(ifbwRow);
        m_ifbw->setDecimals(0);
        m_ifbwUnit = new QComboBox(ifbwRow);
        m_ifbwUnit->addItem(QStringLiteral("Гц"));
        m_ifbwUnit->addItem(QStringLiteral("кГц"));
        m_ifbwUnit->setCurrentIndex(0);
        lay->addWidget(m_ifbw, 1);
        lay->addWidget(m_ifbwUnit);
    }

    m_power = new QDoubleSpinBox(params);
    m_power->setRange(-60.0, 10.0);
    m_power->setDecimals(1);
    m_power->setSuffix(QStringLiteral(" дБм"));
    m_averages = new QSpinBox(params);
    m_averages->setRange(1, 1024);

    form->addRow(QStringLiteral("f нач."), startRow);
    form->addRow(QStringLiteral("f кон."), stopRow);
    form->addRow(QStringLiteral("Точки"), m_points);
    form->addRow(QStringLiteral("ПЧ (IFBW)"), ifbwRow);
    form->addRow(QStringLiteral("Мощность"), m_power);
    form->addRow(QStringLiteral("Усреднение"), m_averages);

    connect(m_fStart, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &MeasureTab::onFreqSpinChanged);
    connect(m_fStop, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &MeasureTab::onFreqSpinChanged);
    connect(m_fStartUnit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onFreqUnitChanged);
    connect(m_fStopUnit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onFreqUnitChanged);
    connect(m_ifbw, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &MeasureTab::onIfbwSpinChanged);
    connect(m_ifbwUnit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onIfbwUnitChanged);
    connect(m_points, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { markSweepSettingsChanged(); });
    connect(m_power, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this] { markSweepSettingsChanged(); });
    connect(m_averages, qOverload<int>(&QSpinBox::valueChanged), this,
            [this] { markSweepSettingsChanged(); });

    m_plotState = new QLabel(QStringLiteral("S-параметры: состояние ещё не выбрано"), page);
    m_plotState->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700;"));
    m_current = new QLabel(QStringLiteral("Канал: —  Att: —  Фаза: —"), page);

    m_nextHint = new QLabel(page);
    m_nextHint->setWordWrap(true);
    m_nextHint->setObjectName(QStringLiteral("hintLabel"));

    m_measureNow = new QPushButton(QStringLiteral("Применить и измерить S11/S21/S12/S22"), page);
    m_measureNow->setObjectName(QStringLiteral("btnPrimary"));
    m_measureNow->setToolTip(
        QStringLiteral("Один свип S11…S22 без перебора DUT. Idle/Ready; во время серии — нет."));
    connect(m_measureNow, &QPushButton::clicked, this, &MeasureTab::measureNowRequested);

    m_progress = new QProgressBar(page);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_counter = new QLabel(QStringLiteral("Состояния: 0 / 0"), page);
    m_eta = new QLabel(QStringLiteral("ETA: —"), page);

    center->addWidget(params);
    center->addWidget(m_measureNow);
    center->addWidget(m_plotState);
    center->addWidget(m_current);
    center->addWidget(m_nextHint);
    center->addStretch(1);
    center->addWidget(m_progress);
    center->addWidget(m_counter);
    center->addWidget(m_eta);
    return page;
}

QWidget* MeasureTab::makeDirectPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 5. Прямая LUT (FR-13)"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_directSummary = new QLabel(page);
    m_directSummary->setWordWrap(true);
    m_directSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_lutPlotCaption = new QLabel(QStringLiteral("Превью LUT (скрыто без данных)"), page);
    m_lutPlotCaption->setObjectName(QStringLiteral("hintLabel"));
    m_lutPlot = new S21PlotWidget(page);
    m_lutPlot->setMinimumHeight(140);
    m_lutPlot->setPanelTitles(QStringLiteral("|S21| из LUT (дБ)"),
                              QStringLiteral("ошибка фазы (°)"));
    m_lutPlot->setVisible(false);
    m_lutPlotCaption->setVisible(false);
    auto* fragTitle = new QLabel(QStringLiteral("Сырой фрагмент"), page);
    fragTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_directFragment = new QLabel(page);
    m_directFragment->setWordWrap(true);
    m_directFragment->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_directFragment->setObjectName(QStringLiteral("hintLabel"));
    layout->addWidget(title);
    layout->addWidget(m_directSummary);
    layout->addWidget(m_lutPlotCaption);
    layout->addWidget(m_lutPlot, 1);
    layout->addWidget(fragTitle);
    layout->addWidget(m_directFragment);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeInversePage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 6. Обратная LUT (FR-14)"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_inverseSummary = new QLabel(page);
    m_inverseSummary->setWordWrap(true);
    m_inverseSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* fragTitle = new QLabel(QStringLiteral("Сырой фрагмент"), page);
    fragTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_inverseFragment = new QLabel(page);
    m_inverseFragment->setWordWrap(true);
    m_inverseFragment->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_inverseFragment->setObjectName(QStringLiteral("hintLabel"));
    layout->addWidget(title);
    layout->addWidget(m_inverseSummary);
    layout->addWidget(fragTitle);
    layout->addWidget(m_inverseFragment);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeValidationPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 7. Валидация и манифест"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_validSummary = new QLabel(page);
    m_validSummary->setWordWrap(true);
    m_validSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* fragTitle = new QLabel(QStringLiteral("Манифест / PDF (фрагмент)"), page);
    fragTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    m_validFragment = new QLabel(page);
    m_validFragment->setWordWrap(true);
    m_validFragment->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_validFragment->setObjectName(QStringLiteral("hintLabel"));
    layout->addWidget(title);
    layout->addWidget(m_validSummary);
    layout->addWidget(fragTitle);
    layout->addWidget(m_validFragment);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeGraphsPage()
{
    auto* page = new QWidget(this);
    auto* root = new QHBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);

    auto* plotHost = new QWidget(page);
    m_graphGrid = new QGridLayout(plotHost);
    m_graphGrid->setContentsMargins(0, 0, 0, 0);
    m_graphGrid->setSpacing(4);
    for (int i = 0; i < static_cast<int>(m_graphPlots.size()); ++i) {
        auto* plot = new S21PlotWidget(plotHost);
        plot->setObjectName(QStringLiteral("graphPlot%1").arg(i));
        plot->setMinimumHeight(180);
        plot->setPanelTitles(QStringLiteral("Модуль (дБ)"), QStringLiteral("Фаза unwrap (°)"));
        plot->setEmptyHint(QStringLiteral(
            "Нет данных — выполните «Измерить сейчас» или запустите перебор кодов"));
        connect(plot, &S21PlotWidget::markerRequested, this, &MeasureTab::addGraphMarker);
        m_graphPlots[i] = plot;
    }

    auto* side = new QWidget(page);
    side->setFixedWidth(330);
    auto* sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(8, 0, 0, 0);

    auto* readLive = new QPushButton(QStringLiteral("Подключить C2220 и измерить всё"), side);
    readLive->setObjectName(QStringLiteral("btnPrimary"));
    readLive->setToolTip(QStringLiteral(
        "Проверить связь и измерить S11/S21/S12/S22 по настройкам нашей программы"));
    connect(readLive, &QPushButton::clicked, this, &MeasureTab::connectAndMeasureRequested);

    auto* measureAll = new QPushButton(QStringLiteral("Применить настройки и измерить всё"), side);
    measureAll->setToolTip(QStringLiteral(
        "Считать S11, S21, S12 и S22; КСВН-1 вычисляется из S11, КСВН-2 — из S22"));
    connect(measureAll, &QPushButton::clicked, this, &MeasureTab::measureNowRequested);

    auto* openSweepSettings = new QPushButton(
        QStringLiteral("Настройки частоты, точек и ПЧ"), side);
    connect(openSweepSettings, &QPushButton::clicked, this,
            [this] { setStageHighlight(3); });

    m_graphDataStatus = new QLabel(QStringLiteral("Реальные данные прибора ещё не получены"), side);
    m_graphDataStatus->setWordWrap(true);
    m_graphDataStatus->setObjectName(QStringLiteral("hintLabel"));

    auto* traces = new QGroupBox(QStringLiteral("Показать графики"), side);
    auto* tracesLayout = new QGridLayout(traces);
    for (int i = 0; i < static_cast<int>(m_graphTraceButtons.size()); ++i) {
        auto* button = new QPushButton(kGraphNames[i], traces);
        button->setObjectName(QStringLiteral("graphTrace%1").arg(kGraphNames[i]));
        button->setCheckable(true);
        button->setChecked(i == 1); // По умолчанию привычный S21.
        button->setStyleSheet(QStringLiteral(
            "QPushButton { border-left: 5px solid %1; } QPushButton:checked { font-weight: 700; }")
                                  .arg(kGraphColors[i].name()));
        connect(button, &QPushButton::toggled, this, [this, i](bool checked) {
            if (checked) {
                const int first = i < 4 ? 4 : 0;
                const int last = i < 4 ? 6 : 4;
                for (int other = first; other < last; ++other) {
                    const QSignalBlocker blocker(m_graphTraceButtons[other]);
                    m_graphTraceButtons[other]->setChecked(false);
                }
            }
            refreshGraphsPage();
            if (checked && m_graphMag[i].isEmpty()) {
                emit measureNowRequested();
            }
        });
        tracesLayout->addWidget(button, i / 2, i % 2);
        m_graphTraceButtons[i] = button;
    }
    m_graphMode = new QPushButton(QStringLiteral("Отдельно"), traces);
    m_graphMode->setToolTip(QStringLiteral("Переключить наложение и отдельные окна"));
    connect(m_graphMode, &QPushButton::clicked, this, [this] {
        m_graphSeparate = !m_graphSeparate;
        refreshGraphsPage();
    });
    tracesLayout->addWidget(m_graphMode, 3, 0, 1, 2);
    auto* resetScale = new QPushButton(QStringLiteral("Показать весь диапазон"), traces);
    connect(resetScale, &QPushButton::clicked, this, [this] {
        for (auto* plot : m_graphPlots) {
            plot->resetView();
        }
    });
    tracesLayout->addWidget(resetScale, 4, 0, 1, 2);

    auto* markers = new QGroupBox(QStringLiteral("Маркеры"), side);
    auto* markersLayout = new QGridLayout(markers);
    m_graphMarkerMode = new QPushButton(QStringLiteral("Ставить маркеры"), markers);
    m_graphMarkerMode->setCheckable(true);
    connect(m_graphMarkerMode, &QPushButton::toggled, this, [this](bool enabled) {
        m_graphMarkerMode->setText(enabled ? QStringLiteral("Маркеры: ВКЛ")
                                           : QStringLiteral("Ставить маркеры"));
        for (auto* plot : m_graphPlots) {
            plot->setMarkerPlacementEnabled(enabled);
        }
    });
    auto* removeMarker = new QPushButton(QStringLiteral("Удалить последний"), markers);
    auto* clearMarkers = new QPushButton(QStringLiteral("Очистить"), markers);
    connect(removeMarker, &QPushButton::clicked, this, [this] {
        if (!m_graphMarkers.isEmpty()) {
            m_graphMarkers.removeLast();
            for (auto* plot : m_graphPlots) {
                plot->setMarkerFrequencies(m_graphMarkers);
            }
            refreshMarkerTerminal();
        }
    });
    connect(clearMarkers, &QPushButton::clicked, this, [this] {
        m_graphMarkers.clear();
        for (auto* plot : m_graphPlots) {
            plot->setMarkerFrequencies(m_graphMarkers);
        }
        refreshMarkerTerminal();
    });
    markersLayout->addWidget(m_graphMarkerMode, 0, 0, 1, 2);
    markersLayout->addWidget(removeMarker, 1, 0);
    markersLayout->addWidget(clearMarkers, 1, 1);

    auto* terminalTitle = new QLabel(
        QStringLiteral("Значения маркеров и калькулятор"), side);
    terminalTitle->setStyleSheet(QStringLiteral("font-weight: 700;"));
    m_graphTerminal = new QPlainTextEdit(side);
    m_graphTerminal->setReadOnly(true);
    m_graphTerminal->setMinimumHeight(220);
    m_graphTerminal->setObjectName(QStringLiteral("graphMarkerTerminal"));
    m_graphExpression = new QLineEdit(side);
    m_graphExpression->setPlaceholderText(QStringLiteral("M2.S21.db - M1.S21.db"));
    auto* calculate = new QPushButton(QStringLiteral("Вычислить"), side);
    connect(calculate, &QPushButton::clicked, this, &MeasureTab::calculateGraphExpression);
    connect(m_graphExpression, &QLineEdit::returnPressed,
            this, &MeasureTab::calculateGraphExpression);

    auto* calcHint = new QLabel(
        QStringLiteral("Примеры: M1.freq, M1.S21.db, M1.S21.phase, M1.VSWR1, 10 / 2"), side);
    calcHint->setWordWrap(true);
    calcHint->setObjectName(QStringLiteral("hintLabel"));

    sideLayout->addWidget(readLive);
    sideLayout->addWidget(measureAll);
    sideLayout->addWidget(openSweepSettings);
    sideLayout->addWidget(m_graphDataStatus);
    sideLayout->addWidget(traces);
    sideLayout->addWidget(markers);
    sideLayout->addWidget(terminalTitle);
    sideLayout->addWidget(m_graphTerminal, 1);
    sideLayout->addWidget(m_graphExpression);
    sideLayout->addWidget(calculate);
    sideLayout->addWidget(calcHint);

    root->addWidget(plotHost, 1);
    root->addWidget(side);
    refreshGraphsPage();
    return page;
}

void MeasureTab::refreshGraphsPage()
{
    QVector<int> selected;
    for (int i = 0; i < static_cast<int>(m_graphTraceButtons.size()); ++i) {
        if (m_graphTraceButtons[i] != nullptr && m_graphTraceButtons[i]->isChecked()) {
            selected.push_back(i);
        }
    }
    if (selected.size() < 2) {
        m_graphSeparate = false;
    }
    const bool vswrMode = !selected.isEmpty() && selected.first() >= 4;
    m_graphMode->setEnabled(selected.size() >= 2);
    m_graphMode->setText(m_graphSeparate ? QStringLiteral("Вместе")
                                         : QStringLiteral("Отдельно"));

    for (auto* plot : m_graphPlots) {
        m_graphGrid->removeWidget(plot);
        plot->setVisible(false);
        plot->setMarkerFrequencies(m_graphMarkers);
        plot->setSinglePanelMode(vswrMode);
        plot->setPanelTitles(vswrMode ? QStringLiteral("КСВН")
                                     : QStringLiteral("Модуль (дБ)"),
                             QStringLiteral("Фаза unwrap (°)"));
    }

    auto makeTrace = [this](int index) {
        S21PlotTrace trace;
        trace.name = kGraphNames[index];
        trace.freqGhz = m_graphFreqGhz[index];
        trace.magDb = m_graphMag[index];
        trace.phaseDeg = m_graphPhase[index];
        trace.color = kGraphColors[index];
        return trace;
    };

    if (!m_graphSeparate) {
        QVector<S21PlotTrace> traces;
        for (int index : selected) {
            traces.push_back(makeTrace(index));
        }
        m_graphPlots[0]->setSubtitle(
            selected.size() > 1 ? QStringLiteral("Совмещённые трассы")
                                : (selected.isEmpty() ? QString() : kGraphNames[selected.first()]));
        m_graphPlots[0]->setTraces(traces);
        m_graphGrid->addWidget(m_graphPlots[0], 0, 0);
        m_graphPlots[0]->setVisible(true);
    } else {
        for (int slot = 0; slot < selected.size(); ++slot) {
            const int index = selected[slot];
            m_graphPlots[slot]->setSubtitle(kGraphNames[index]);
            m_graphPlots[slot]->setTraces({makeTrace(index)});
            m_graphGrid->addWidget(m_graphPlots[slot], slot / 2, slot % 2);
            m_graphPlots[slot]->setVisible(true);
        }
    }
    refreshMarkerTerminal();
}

void MeasureTab::addGraphMarker(double freqGhz)
{
    const QVector<double>* axis = nullptr;
    for (int trace = 0; trace < static_cast<int>(m_graphTraceButtons.size()); ++trace) {
        if (m_graphTraceButtons[trace]->isChecked() && !m_graphFreqGhz[trace].isEmpty()) {
            axis = &m_graphFreqGhz[trace];
            break;
        }
    }
    if (axis == nullptr) {
        return;
    }
    const qsizetype index = nearestFrequencyIndex(*axis, freqGhz);
    if (index < 0) {
        return;
    }
    const double snapped = (*axis)[index];
    if (std::none_of(m_graphMarkers.cbegin(), m_graphMarkers.cend(), [snapped](double value) {
            return std::abs(value - snapped) < 1e-12;
        })) {
        m_graphMarkers.push_back(snapped);
        for (auto* plot : m_graphPlots) {
            plot->setMarkerFrequencies(m_graphMarkers);
        }
        refreshMarkerTerminal();
    }
}

void MeasureTab::refreshMarkerTerminal()
{
    QStringList lines;
    QStringList automatic;
    for (int trace = 0; trace < static_cast<int>(m_graphTraceButtons.size()); ++trace) {
        if (!m_graphTraceButtons[trace]->isChecked()) {
            continue;
        }
        const auto statistics = plotTraceStatistics({
            kGraphNames[trace], m_graphFreqGhz[trace], m_graphMag[trace],
            m_graphPhase[trace], kGraphColors[trace]});
        if (!statistics.valid) {
            continue;
        }
        const QString unit = trace >= 4 ? QString() : QStringLiteral(" дБ");
        automatic << QStringLiteral("%1 MIN: %2%3 @ %4 ГГц")
                         .arg(kGraphNames[trace])
                         .arg(statistics.minValue, 0, 'f', 4)
                         .arg(unit)
                         .arg(statistics.minFrequencyGhz, 0, 'f', 6)
                  << QStringLiteral("%1 MAX: %2%3 @ %4 ГГц")
                         .arg(kGraphNames[trace])
                         .arg(statistics.maxValue, 0, 'f', 4)
                         .arg(unit)
                         .arg(statistics.maxFrequencyGhz, 0, 'f', 6)
                  << QStringLiteral("%1 СРЕДНЕЕ: %2%3 @ %4 ГГц")
                         .arg(kGraphNames[trace])
                         .arg(statistics.averageValue, 0, 'f', 4)
                         .arg(unit)
                         .arg(statistics.averageFrequencyGhz, 0, 'f', 6);
    }
    if (!automatic.isEmpty()) {
        lines << QStringLiteral("Автоматические маркеры:") << automatic << QString();
    }
    if (m_graphMarkers.isEmpty()) {
        lines << QStringLiteral("Включите «Ставить маркеры» и щёлкните по графику.");
    }
    for (qsizetype marker = 0; marker < m_graphMarkers.size(); ++marker) {
        lines << QStringLiteral("M%1: %2 ГГц (%3 МГц)")
                     .arg(marker + 1)
                     .arg(m_graphMarkers[marker], 0, 'f', 6)
                     .arg(m_graphMarkers[marker] * 1000.0, 0, 'f', 3);
        for (int trace = 0; trace < static_cast<int>(m_graphTraceButtons.size()); ++trace) {
            const qsizetype point = nearestFrequencyIndex(m_graphFreqGhz[trace],
                                                           m_graphMarkers[marker]);
            if (!m_graphTraceButtons[trace]->isChecked() || point < 0
                || point >= m_graphMag[trace].size()) {
                continue;
            }
            if (trace >= 4) {
                const double value = m_graphMag[trace][point];
                lines << (std::isfinite(value)
                    ? QStringLiteral("  %1: %2").arg(kGraphNames[trace]).arg(value, 0, 'f', 4)
                    : QStringLiteral("  %1: ∞ (|Γ| ≥ 1)").arg(kGraphNames[trace]));
                continue;
            }
            if (point >= m_graphPhase[trace].size()) {
                continue;
            }
            lines << QStringLiteral("  %1: %2 дБ; %3°")
                         .arg(kGraphNames[trace])
                         .arg(m_graphMag[trace][point], 0, 'f', 4)
                         .arg(m_graphPhase[trace][point], 0, 'f', 3);
        }
    }
    if (!m_graphCalculations.isEmpty()) {
        lines << QString() << QStringLiteral("Расчёты:") << m_graphCalculations;
    }
    m_graphTerminal->setPlainText(lines.join(QLatin1Char('\n')));
    m_graphTerminal->verticalScrollBar()->setValue(m_graphTerminal->verticalScrollBar()->maximum());
}

double MeasureTab::graphOperandValue(const QString& token, bool* ok) const
{
    QString normalized = token.trimmed();
    normalized.replace(QLatin1Char(','), QLatin1Char('.'));
    bool numberOk = false;
    const double number = normalized.toDouble(&numberOk);
    if (numberOk) {
        *ok = true;
        return number;
    }

    static const QRegularExpression vswrPattern(
        QStringLiteral("^M(\\d+)\\.VSWR([12])$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto vswrMatch = vswrPattern.match(normalized);
    if (vswrMatch.hasMatch()) {
        const int marker = vswrMatch.captured(1).toInt() - 1;
        const int trace = 3 + vswrMatch.captured(2).toInt();
        if (marker < 0 || marker >= m_graphMarkers.size()) {
            *ok = false;
            return 0.0;
        }
        const qsizetype point = nearestFrequencyIndex(m_graphFreqGhz[trace],
                                                       m_graphMarkers[marker]);
        if (point < 0 || point >= m_graphMag[trace].size()) {
            *ok = false;
            return 0.0;
        }
        *ok = true;
        return m_graphMag[trace][point];
    }

    static const QRegularExpression markerPattern(
        QStringLiteral("^M(\\d+)\\.(FREQ|S(11|21|12|22)\\.(DB|PHASE))$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = markerPattern.match(normalized);
    if (!match.hasMatch()) {
        *ok = false;
        return 0.0;
    }
    const int marker = match.captured(1).toInt() - 1;
    if (marker < 0 || marker >= m_graphMarkers.size()) {
        *ok = false;
        return 0.0;
    }
    if (match.captured(2).compare(QStringLiteral("FREQ"), Qt::CaseInsensitive) == 0) {
        *ok = true;
        return m_graphMarkers[marker];
    }

    const QString sName = QStringLiteral("S") + match.captured(3);
    const auto nameIt = std::find(kGraphNames.cbegin(), kGraphNames.cend(), sName.toUpper());
    if (nameIt == kGraphNames.cend()) {
        *ok = false;
        return 0.0;
    }
    const int trace = std::distance(kGraphNames.cbegin(), nameIt);
    const qsizetype point = nearestFrequencyIndex(m_graphFreqGhz[trace],
                                                   m_graphMarkers[marker]);
    const bool phase = match.captured(4).compare(QStringLiteral("PHASE"), Qt::CaseInsensitive) == 0;
    const auto& values = phase ? m_graphPhase[trace] : m_graphMag[trace];
    if (point < 0 || point >= values.size()) {
        *ok = false;
        return 0.0;
    }
    *ok = true;
    return values[point];
}

void MeasureTab::calculateGraphExpression()
{
    const QString expression = m_graphExpression->text().trimmed();
    if (expression.isEmpty()) {
        return;
    }
    static const QRegularExpression binary(
        QStringLiteral("^\\s*(\\S+)\\s+([+\\-*/])\\s+(\\S+)\\s*$"));
    const auto match = binary.match(expression);
    bool leftOk = false;
    bool rightOk = false;
    double result = 0.0;
    QString error;
    if (!match.hasMatch()) {
        result = graphOperandValue(expression, &leftOk);
        if (!leftOk) {
            error = QStringLiteral("ожидается значение или выражение с пробелами: A + B");
        }
    } else {
        const double left = graphOperandValue(match.captured(1), &leftOk);
        const double right = graphOperandValue(match.captured(3), &rightOk);
        if (!leftOk || !rightOk) {
            error = QStringLiteral("неизвестный маркер или значение");
        } else if (match.captured(2) == QStringLiteral("+")) {
            result = left + right;
        } else if (match.captured(2) == QStringLiteral("-")) {
            result = left - right;
        } else if (match.captured(2) == QStringLiteral("*")) {
            result = left * right;
        } else if (std::abs(right) < 1e-15) {
            error = QStringLiteral("деление на ноль");
        } else {
            result = left / right;
        }
    }

    m_graphCalculations << (error.isEmpty()
        ? QStringLiteral("> %1\n= %2").arg(expression).arg(result, 0, 'g', 12)
        : QStringLiteral("> %1\nОшибка: %2").arg(expression, error));
    m_graphExpression->clear();
    refreshMarkerTerminal();
}

void MeasureTab::onStageClicked(int row)
{
    if (row >= 0 && row < m_stack->count()) {
        m_stack->setCurrentIndex(row);
    }
}

void MeasureTab::applyRunConfigDefaults(double fStartHz,
                                        double fStopHz,
                                        int points,
                                        int ifbwHz,
                                        double powerDbm,
                                        int averages)
{
    m_fStartHz = fStartHz;
    m_fStopHz = fStopHz;
    m_ifbwHz = ifbwHz > 0 ? ifbwHz : 1000;
    // Для диапазона ~ГГц удобнее МГц; иначе ГГц.
    const int unit = (fStartHz >= 1e8 && fStopHz < 1e11) ? 2 : 3;
    {
        const QSignalBlocker b1(m_fStartUnit);
        const QSignalBlocker b2(m_fStopUnit);
        m_fStartUnit->setCurrentIndex(unit);
        m_fStopUnit->setCurrentIndex(unit);
    }
    syncFreqSpinsFromHz();
    {
        const QSignalBlocker b(m_ifbwUnit);
        m_ifbwUnit->setCurrentIndex(m_ifbwHz >= 1000 && (m_ifbwHz % 1000) == 0 ? 1 : 0);
    }
    syncIfbwSpinFromHz();
    const QSignalBlocker pointsBlocker(m_points);
    const QSignalBlocker powerBlocker(m_power);
    const QSignalBlocker averagesBlocker(m_averages);
    m_points->setValue(points);
    m_power->setValue(powerDbm);
    m_averages->setValue(averages);
}

void MeasureTab::setStageHighlight(int stageIndex0)
{
    if (stageIndex0 >= 0 && stageIndex0 < m_stages->count()) {
        m_stages->setCurrentRow(stageIndex0);
    }
}

void MeasureTab::setProgress(qint64 completed,
                             qint64 total,
                             int channel,
                             int attCode,
                             int phaseCode)
{
    if (total <= 0) {
        m_progress->setValue(0);
    } else {
        m_progress->setValue(static_cast<int>((completed * 100) / total));
    }
    m_counter->setText(QStringLiteral("Состояния: %1 / %2").arg(completed).arg(total));
    m_current->setText(QStringLiteral("Канал: %1  Att: %2  Фаза: %3")
                           .arg(channel)
                           .arg(attCode)
                           .arg(phaseCode));
    if (channel < 0 && attCode < 0 && phaseCode < 0) {
        m_plotState->setText(QStringLiteral("S-параметры: состояние ещё не выбрано"));
    } else {
        m_plotState->setText(QStringLiteral("S-параметры: ch=%1  att=%2  ph=%3")
                                 .arg(channel)
                                 .arg(attCode)
                                 .arg(phaseCode));
    }
}

void MeasureTab::setEtaText(const QString& text)
{
    m_eta->setText(text);
}

void MeasureTab::setSweepCurves(const QVector<double>& freqGhz,
                                const QVector<double>& magDb,
                                const QVector<double>& phaseUnwrapDeg)
{
    // Не затирать последний хороший график пустым/повреждённым ответом прибора.
    if (freqGhz.size() < 2 || freqGhz.first() >= freqGhz.last()
        || magDb.size() != freqGhz.size() || phaseUnwrapDeg.size() != freqGhz.size()) {
        return;
    }
    m_graphFreqGhz[1] = freqGhz;
    m_graphMag[1] = magDb;
    m_graphPhase[1] = phaseUnwrapDeg;
    m_plotState->setText(QStringLiteral(
        "S-параметры: сохранена последняя корректная реальная трасса S21"));
    m_graphDataStatus->setText(
        QStringLiteral("Реальные данные прибора: S21, %1 точек, %2…%3 ГГц")
            .arg(freqGhz.size())
            .arg(freqGhz.first(), 0, 'f', 6)
            .arg(freqGhz.last(), 0, 'f', 6));
    refreshGraphsPage();
}

void MeasureTab::setSparamsCurves(const QVector<double>& freqGhz,
                                  const QVector<double>& s11mag,
                                  const QVector<double>& s11ph,
                                  const QVector<double>& s21mag,
                                  const QVector<double>& s21ph,
                                  const QVector<double>& s12mag,
                                  const QVector<double>& s12ph,
                                  const QVector<double>& s22mag,
                                  const QVector<double>& s22ph)
{
    const auto completeTrace = [&freqGhz](const QVector<double>& mag,
                                          const QVector<double>& phase) {
        return mag.size() == freqGhz.size() && phase.size() == freqGhz.size();
    };
    if (freqGhz.size() < 2 || freqGhz.first() >= freqGhz.last()
        || !(completeTrace(s11mag, s11ph) || completeTrace(s21mag, s21ph)
             || completeTrace(s12mag, s12ph) || completeTrace(s22mag, s22ph))) {
        return;
    }
    const auto storeTrace = [this, &freqGhz, &completeTrace](
                                int index, const QVector<double>& mag,
                                const QVector<double>& phase) {
        if (!completeTrace(mag, phase)) {
            return;
        }
        m_graphFreqGhz[index] = freqGhz;
        m_graphMag[index] = mag;
        m_graphPhase[index] = phase;
    };
    storeTrace(0, s11mag, s11ph);
    storeTrace(1, s21mag, s21ph);
    storeTrace(2, s12mag, s12ph);
    storeTrace(3, s22mag, s22ph);
    if (completeTrace(s11mag, s11ph)) {
        m_graphFreqGhz[4] = freqGhz;
        m_graphMag[4] = vswrFromReturnLossDb(s11mag);
        m_graphPhase[4].clear();
    }
    if (completeTrace(s22mag, s22ph)) {
        m_graphFreqGhz[5] = freqGhz;
        m_graphMag[5] = vswrFromReturnLossDb(s22mag);
        m_graphPhase[5].clear();
    }
    m_plotState->setText(QStringLiteral(
        "S-параметры: сохранены последние корректные реальные данные прибора"));
    QStringList updated;
    if (completeTrace(s11mag, s11ph)) updated << QStringLiteral("S11");
    if (completeTrace(s21mag, s21ph)) updated << QStringLiteral("S21");
    if (completeTrace(s12mag, s12ph)) updated << QStringLiteral("S12");
    if (completeTrace(s22mag, s22ph)) updated << QStringLiteral("S22");
    m_graphDataStatus->setText(
        QStringLiteral("Реальные данные прибора обновлены: %1; %2 точек, %3…%4 ГГц")
            .arg(updated.join(QStringLiteral(", ")))
            .arg(freqGhz.size())
            .arg(freqGhz.first(), 0, 'f', 6)
            .arg(freqGhz.last(), 0, 'f', 6));
    refreshGraphsPage();
}

void MeasureTab::setVnaCalibrationIdHint(const QString& id)
{
    m_vnaCalId = id.trimmed();
    refreshCalPageText();
}

void MeasureTab::refreshCalPageText()
{
    const QString idLine = m_vnaCalId.isEmpty()
        ? QStringLiteral("vna_calibration_id: (пусто — допустимо)")
        : QStringLiteral("vna_calibration_id: %1").arg(m_vnaCalId);
    m_calText->setText(
        QStringLiteral(
            "Статус стенда (мастер / SOLT):\n"
            "Подключения: %1\n"
            "IDN C2220 / контроллер: %2\n"
            "Калибровка ВАЦ: %3\n"
            "THRU: %4\n"
            "%5")
            .arg(m_connectionsOk ? QStringLiteral("подтверждены")
                                 : QStringLiteral("ожидают мастера"),
                 m_idnOk ? QStringLiteral("подтверждены") : QStringLiteral("ожидают мастера"),
                 m_calOk ? QStringLiteral("подтверждена (Apply / мастер)")
                         : QStringLiteral("не подтверждена"),
                 m_thruText,
                 idLine));
}

QString MeasureTab::calStepTitle(int step) const
{
    if (isOnePortCal()) {
        const int port = m_calPort ? m_calPort->currentData().toInt() : 1;
        switch (step) {
        case 0:
            return QStringLiteral("Begin — начать однопортовую калибровку (порт %1)").arg(port);
        case 1:
            return QStringLiteral("OPEN — подключите OPEN к порту %1").arg(port);
        case 2:
            return QStringLiteral("SHORT — подключите SHORT к порту %1").arg(port);
        case 3:
            return QStringLiteral("LOAD — подключите LOAD к порту %1").arg(port);
        case 4:
            return QStringLiteral("Apply — применить калибровку");
        default:
            return QStringLiteral("Готово");
        }
    }
    switch (step) {
    case 0:
        return QStringLiteral("Begin — начать двухпортовую калибровку");
    case 1:
        return QStringLiteral("OPEN порт 1 — подключите OPEN к порту 1");
    case 2:
        return QStringLiteral("SHORT порт 1 — подключите SHORT к порту 1");
    case 3:
        return QStringLiteral("LOAD порт 1 — подключите LOAD к порту 1");
    case 4:
        return QStringLiteral("OPEN порт 2 — подключите OPEN к порту 2");
    case 5:
        return QStringLiteral("SHORT порт 2 — подключите SHORT к порту 2");
    case 6:
        return QStringLiteral("LOAD порт 2 — подключите LOAD к порту 2");
    case 7:
        return QStringLiteral("THRU — соедините порты 1–2");
    case 8:
        return QStringLiteral("Apply — применить калибровку");
    default:
        return QStringLiteral("Готово");
    }
}

bool MeasureTab::isOnePortCal() const
{
    return m_calKind && m_calKind->currentData().toInt() == 0;
}

int MeasureTab::calApplyStep() const
{
    return isOnePortCal() ? 4 : 8;
}

int MeasureTab::calStepCount() const
{
    return calApplyStep() + 1;
}

void MeasureTab::onCalKindChanged(int /*index*/)
{
    const bool one = isOnePortCal();
    if (m_calPortLabel) {
        m_calPortLabel->setVisible(one);
    }
    if (m_calPort) {
        m_calPort->setVisible(one);
        m_calPort->setEnabled(one && !m_soltBusy);
    }
    if (m_calExtraGroup) {
        m_calExtraGroup->setVisible(!one);
    }
    m_soltStep = 0;
    m_soltBusy = false;
    m_calOk = false;
    refreshSoltUi();
    refreshCalPageText();
}

void MeasureTab::refreshSoltUi()
{
    const int apply = calApplyStep();
    if (!m_soltStepLabel || !m_soltConfirm) {
        return;
    }
    const bool one = isOnePortCal();
    const QString kindName = one ? QStringLiteral("OSL") : QStringLiteral("SOLT");
    if (m_calPort) {
        m_calPort->setEnabled(one && !m_soltBusy && m_soltStep == 0);
    }
    if (m_calKind) {
        m_calKind->setEnabled(!m_soltBusy && m_soltStep == 0);
    }
    if (m_soltStep > apply) {
        m_soltStepLabel->setText(
            QStringLiteral("%1: все шаги выполнены (Apply OK)").arg(kindName));
        m_soltHint->setText(
            QStringLiteral("Калибровка применена через текущий VNA-транспорт. "
                           "Доп. чеклист Response/Thru — по желанию (только 2-порт)."));
        m_soltConfirm->setEnabled(false);
        return;
    }
    m_soltStepLabel->setText(QStringLiteral("Шаг %1 / %2: %3")
                                 .arg(m_soltStep + 1)
                                 .arg(calStepCount())
                                 .arg(calStepTitle(m_soltStep)));
    m_soltHint->setText(
        one ? QStringLiteral(
                  "Установите стандарт на выбранный порт, затем нажмите подтверждение. "
                  "Команда уйдёт в MeasureWorker → IVna::calibrate_one_port (SOLT1).")
            : QStringLiteral(
                  "Установите стандарт / соединение, затем нажмите подтверждение. "
                  "Команда уйдёт в MeasureWorker → IVna::calibrate_two_port (SOLT2)."));
    m_soltConfirm->setEnabled(!m_soltBusy);
}

void MeasureTab::onSoltConfirmClicked()
{
    const int apply = calApplyStep();
    if (m_soltBusy || m_soltStep > apply) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Подтверждение шага калибровки"),
        QStringLiteral("Выполнить шаг:\n%1?").arg(calStepTitle(m_soltStep)),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    m_soltBusy = true;
    refreshSoltUi();
    const int kind = isOnePortCal() ? 0 : 1;
    const int port = m_calPort ? m_calPort->currentData().toInt() : 1;
    emit calibrateStepRequested(kind, m_soltStep, port);
}

void MeasureTab::onSoltResetClicked()
{
    m_soltStep = 0;
    m_soltBusy = false;
    m_calOk = false;
    refreshSoltUi();
    refreshCalPageText();
}

void MeasureTab::setMeasureNowEnabled(bool enabled)
{
    if (m_measureNow) {
        m_measureNow->setEnabled(enabled);
    }
}

void MeasureTab::onCalibrateStepFinished(bool ok, int step, const QString& message)
{
    m_soltBusy = false;
    const int apply = calApplyStep();
    if (ok && step == m_soltStep) {
        if (step == apply) {
            m_calOk = true;
            m_soltStep = apply + 1;
        } else {
            ++m_soltStep;
        }
    } else if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Калибровка ВАЦ"),
                             message.isEmpty() ? QStringLiteral("Шаг не выполнен") : message);
    }
    refreshSoltUi();
    refreshCalPageText();
}

void MeasureTab::setStandCheck(bool connectionsOk,
                               bool idnOk,
                               bool calOk,
                               const QString& thruText,
                               bool noOverload,
                               bool probeOk,
                               double powerDbm,
                               bool engineer)
{
    m_connectionsOk = connectionsOk;
    m_idnOk = idnOk;
    m_calOk = calOk;
    m_thruText = thruText;
    refreshCalPageText();

    m_linText->setText(
        QStringLiteral(
            "Линейность — контроль мощности и отсутствия перегрузки приёмника "
            "до полного перебора кодов.\n\n"
            "Мощность: %1 дБм\n"
            "Перегрузка: %2\n"
            "Пробные коды: %3\n"
            "Инженерный профиль: %4\n\n"
            "Опасные мощность / direct access / safe_state — только инженеру (UI-06).")
            .arg(powerDbm, 0, 'f', 1)
            .arg(noOverload ? QStringLiteral("не обнаружена (подтверждено)")
                            : QStringLiteral("ещё не подтверждена"))
            .arg(probeOk ? QStringLiteral("разрешены") : QStringLiteral("ожидают мастера"))
            .arg(engineer ? QStringLiteral("включён") : QStringLiteral("выключен")));
}

void MeasureTab::setSeriesArtifacts(const QString& seriesRoot,
                                    const QString& directLut,
                                    const QString& inverseLut,
                                    const QString& report,
                                    const QString& manifest)
{
    m_directSummary->setText(
        QStringLiteral(
            "Прямая LUT пишется при Finalizing → Complete (FR-13).\n\n"
            "Серия: %1\n"
            "Файл: %2\n\n"
            "После Complete здесь появится сводка valid/total и превью кривой.")
            .arg(seriesRoot, directLut));
    m_directFragment->setText(QStringLiteral("(фрагмент появится после Complete)"));
    m_inverseSummary->setText(
        QStringLiteral(
            "Обратная LUT подбирает пару кодов под цель по минимуму J (FR-14).\n\n"
            "Серия: %1\n"
            "Файл: %2")
            .arg(seriesRoot, inverseLut));
    m_inverseFragment->setText(QStringLiteral("(фрагмент появится после Complete)"));
    m_validSummary->setText(
        QStringLiteral(
            "Итог серии появится после Complete: completed_states, valid, пути report/manifest.\n\n"
            "Стартовые пороги QC (не аттестация): дрейф фазы 1,0°; "
            "остаток обратной LUT 2,8125°; повторяемость 0,10 дБ / 1,0°.\n\n"
            "Серия: %1\n"
            "Протокол: %2\n"
            "Манифест: %3")
            .arg(seriesRoot, report, manifest));
    m_validFragment->setText(QStringLiteral("(манифест/PDF — после Complete)"));
    m_lutPlot->setVisible(false);
    m_lutPlotCaption->setVisible(false);
}

void MeasureTab::applySeriesArtifactsPreview(const QString& runId,
                                             qint64 completedStates,
                                             const QString& directPath,
                                             qint64 directValid,
                                             qint64 directTotal,
                                             bool directFlat,
                                             const QString& directFragment,
                                             const QString& inversePath,
                                             qint64 inverseValid,
                                             qint64 inverseTotal,
                                             bool inverseFlat,
                                             const QString& inverseFragment,
                                             const QString& reportPath,
                                             qint64 reportValid,
                                             const QString& reportFragment,
                                             const QString& manifestPath,
                                             qint64 manifestLines,
                                             const QString& manifestFragment)
{
    const auto fmtCount = [](qint64 v, qint64 t) -> QString {
        if (v < 0 || t < 0) {
            return QStringLiteral("не прочитано");
        }
        return QStringLiteral("%1 / %2").arg(v).arg(t);
    };

    QString directExtra;
    if (directFlat) {
        directExtra = QStringLiteral(
            "\n⚠ Профиль похож на имитатор (mag≈0 / плоский S21) — это не метрология стенда.");
    }
    m_directSummary->setText(
        QStringLiteral(
            "Прямая LUT — таблица измеренных S21 по состояниям (не график текущего свипа).\n\n"
            "run_id: %1\n"
            "Путь: %2\n"
            "valid / total: %3\n"
            "Смысл: модуль, развёрнутая фаза, дрейф, повторяемость, флаг valid (FR-13).%4")
            .arg(runId, directPath, fmtCount(directValid, directTotal), directExtra));
    m_directFragment->setText(directFragment.isEmpty()
                                  ? QStringLiteral("(пусто)")
                                  : directFragment);

    QString inverseExtra;
    if (inverseFlat) {
        inverseExtra = QStringLiteral(
            "\n⚠ Остатки около нуля — типично для имитатора, не для живого тракта.");
    }
    m_inverseSummary->setText(
        QStringLiteral(
            "Обратная LUT — подобранные коды под цели (FR-14).\n\n"
            "run_id: %1\n"
            "Путь: %2\n"
            "valid / total: %3%4")
            .arg(runId, inversePath, fmtCount(inverseValid, inverseTotal), inverseExtra));
    m_inverseFragment->setText(inverseFragment.isEmpty()
                                   ? QStringLiteral("(пусто)")
                                   : inverseFragment);

    const QString closed =
        (completedStates >= 0 && directValid >= 0)
            ? QStringLiteral(
                  "Серия закрыта. QC по порогам профиля выполнен (это не аттестация изделия).")
            : QStringLiteral("Серия завершена; часть артефактов не прочитана — проверьте пути.");
    m_validSummary->setText(
        QStringLiteral(
            "Итог серии\n\n"
            "%1\n\n"
            "run_id: %2\n"
            "completed_states: %3\n"
            "прямая LUT valid: %4\n"
            "обратная LUT valid: %5\n"
            "report: %6\n"
            "manifest: %7 (%8 строк)")
            .arg(closed,
                 runId,
                 completedStates < 0 ? QStringLiteral("—") : QString::number(completedStates),
                 fmtCount(directValid, directTotal),
                 fmtCount(inverseValid, inverseTotal),
                 reportPath,
                 manifestPath,
                 manifestLines < 0 ? QStringLiteral("—") : QString::number(manifestLines)));
    QString frag = reportFragment;
    if (!manifestFragment.isEmpty()) {
        if (!frag.isEmpty()) {
            frag += QStringLiteral("\n---\n");
        }
        frag += manifestFragment;
    }
    Q_UNUSED(reportValid);
    m_validFragment->setText(frag.isEmpty() ? QStringLiteral("(пусто)") : frag);
}

void MeasureTab::applyDirectLutCurvePreview(const QVector<double>& freqGhz,
                                            const QVector<double>& magDb,
                                            const QVector<double>& phaseErrorDeg,
                                            int channel,
                                            int attCode,
                                            int phaseCode)
{
    if (freqGhz.size() < 2 || magDb.size() != freqGhz.size()) {
        m_lutPlot->setVisible(false);
        m_lutPlotCaption->setVisible(false);
        return;
    }
    m_lutPlotCaption->setText(
        QStringLiteral("Превью прямой LUT: ch=%1 att=%2 ph=%3 (|S21| и ошибка фазы)")
            .arg(channel)
            .arg(attCode)
            .arg(phaseCode));
    m_lutPlotCaption->setVisible(true);
    m_lutPlot->setVisible(true);
    m_lutPlot->setSubtitle(QStringLiteral("из файла LUT, не текущий свип"));
    m_lutPlot->setCurves(freqGhz, magDb, phaseErrorDeg);
}

void MeasureTab::setRunStateGuide(int runState)
{
    using afar::RunState;
    const auto st = static_cast<RunState>(runState);
    QString next;
    switch (st) {
    case RunState::Idle:
        next = QStringLiteral("Дальше: откройте мастер запуска (этап 1).");
        break;
    case RunState::Connecting:
    case RunState::SelfTest:
        next = QStringLiteral("Дальше: дождитесь READY после мастера.");
        break;
    case RunState::Ready:
        next = QStringLiteral("Дальше: нажмите «Старт» — начнётся перебор кодов.");
        break;
    case RunState::Running:
    case RunState::Pausing:
        next = QStringLiteral(
            "Дальше: смотрите свип и матрицу; «Пауза» / «Стоп» при необходимости.");
        break;
    case RunState::Paused:
        next = QStringLiteral("Дальше: «Продолжить» или «Стоп».");
        break;
    case RunState::Stopping:
        next = QStringLiteral("Дальше: идёт остановка серии…");
        break;
    case RunState::Finalizing:
        next = QStringLiteral("Дальше: пишутся LUT / отчёт — не закрывайте окно.");
        break;
    case RunState::Complete:
        next = QStringLiteral(
            "Дальше: этапы 5–7 — сводка LUT и итог серии; матрица — ячейки.");
        break;
    case RunState::Aborted:
    case RunState::Error:
    case RunState::Recovery:
        next = QStringLiteral("Дальше: разберите диагностику; при необходимости — новая серия.");
        break;
    }
    m_nextHint->setText(next);
}

void MeasureTab::setUnfinishedSeriesHint(const QString& path)
{
    if (path.isEmpty()) {
        m_unfinished->setText(QStringLiteral("Незакрытая серия не найдена."));
    } else {
        m_unfinished->setText(
            QStringLiteral("Найдена незакрытая серия (нет report.pdf):\n%1\n"
                           "Нажмите «Продолжить незакрытую серию» (FR-12).")
                .arg(path));
    }
}

double MeasureTab::fStartHz() const
{
    return m_fStartHz;
}

double MeasureTab::fStopHz() const
{
    return m_fStopHz;
}

int MeasureTab::points() const
{
    return m_points->value();
}

int MeasureTab::ifbwHz() const
{
    return m_ifbwHz;
}

double MeasureTab::powerDbm() const
{
    return m_power->value();
}

int MeasureTab::averages() const
{
    return m_averages->value();
}

double MeasureTab::freqUnitScale(int unitIndex)
{
    switch (unitIndex) {
    case 0:
        return 1.0;
    case 1:
        return 1e3;
    case 2:
        return 1e6;
    case 3:
        return 1e9;
    default:
        return 1e6;
    }
}

double MeasureTab::ifbwUnitScale(int unitIndex)
{
    return unitIndex == 1 ? 1e3 : 1.0;
}

void MeasureTab::applyFreqSpinLimits(QDoubleSpinBox* spin, int unitIndex) const
{
    // 100 кГц … 40 ГГц в выбранных единицах.
    const double scale = freqUnitScale(unitIndex);
    const double minHz = 1e5;
    const double maxHz = 40e9;
    spin->setRange(minHz / scale, maxHz / scale);
    switch (unitIndex) {
    case 0:
        spin->setDecimals(0);
        break;
    case 1:
        spin->setDecimals(3);
        break;
    case 2:
        spin->setDecimals(3);
        break;
    case 3:
        spin->setDecimals(6);
        break;
    default:
        spin->setDecimals(3);
        break;
    }
}

void MeasureTab::syncFreqSpinsFromHz()
{
    m_freqUiGuard = true;
    applyFreqSpinLimits(m_fStart, m_fStartUnit->currentIndex());
    applyFreqSpinLimits(m_fStop, m_fStopUnit->currentIndex());
    m_fStart->setValue(m_fStartHz / freqUnitScale(m_fStartUnit->currentIndex()));
    m_fStop->setValue(m_fStopHz / freqUnitScale(m_fStopUnit->currentIndex()));
    m_freqUiGuard = false;
}

void MeasureTab::syncIfbwSpinFromHz()
{
    m_freqUiGuard = true;
    const double scale = ifbwUnitScale(m_ifbwUnit->currentIndex());
    m_ifbw->setRange(1.0 / scale, 1e6 / scale);
    m_ifbw->setDecimals(m_ifbwUnit->currentIndex() == 0 ? 0 : 3);
    m_ifbw->setValue(static_cast<double>(m_ifbwHz) / scale);
    m_freqUiGuard = false;
}

void MeasureTab::onFreqSpinChanged()
{
    if (m_freqUiGuard) {
        return;
    }
    m_fStartHz = m_fStart->value() * freqUnitScale(m_fStartUnit->currentIndex());
    m_fStopHz = m_fStop->value() * freqUnitScale(m_fStopUnit->currentIndex());
    markSweepSettingsChanged();
}

void MeasureTab::onFreqUnitChanged()
{
    if (m_freqUiGuard) {
        return;
    }
    // Пересчёт отображения из сохранённых Гц — без потери.
    syncFreqSpinsFromHz();
}

void MeasureTab::onIfbwSpinChanged()
{
    if (m_freqUiGuard) {
        return;
    }
    const double hz = m_ifbw->value() * ifbwUnitScale(m_ifbwUnit->currentIndex());
    m_ifbwHz = static_cast<int>(std::llround(hz));
    if (m_ifbwHz < 1) {
        m_ifbwHz = 1;
    }
    markSweepSettingsChanged();
}

void MeasureTab::markSweepSettingsChanged()
{
    const QString text = QStringLiteral(
        "Настройки изменены. Последний реальный график сохранён; "
        "нажмите «Измерить сейчас» для обновления всех S-параметров.");
    if (m_plotState != nullptr) {
        m_plotState->setText(text);
    }
    if (m_graphDataStatus != nullptr) {
        m_graphDataStatus->setText(text);
    }
}

void MeasureTab::onIfbwUnitChanged()
{
    if (m_freqUiGuard) {
        return;
    }
    syncIfbwSpinFromHz();
}
