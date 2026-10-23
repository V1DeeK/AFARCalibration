#include "MeasureTab.h"

#include "S21PlotWidget.h"
#include "S2VnaGraphHost.h"

#include "../cal/PhaseMath.h"
#include "../measure/RunStateMachine.h"
#include "afar/C2220Limits.h"

#include <QCheckBox>
#include <QAbstractItemView>
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
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QStandardItem>
#include <QStandardItemModel>
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

} // namespace

MeasureTab::MeasureTab(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    // Калибровка и подготовка серии — третья страница верхней области.
    m_calTabs = new QTabWidget(this);
    m_calTabs->setObjectName(QStringLiteral("calibrationTabs"));
    // Вкладки этапов остаются внутри карточки калибровки; настройки свипа вынесены выше.
    m_calTabs->setUsesScrollButtons(false);
    m_calTabs->tabBar()->setElideMode(Qt::ElideNone);
    m_calTabs->addTab(makeCalPage(), QStringLiteral("Калибровка"));
    m_calTabs->addTab(makeLinearityPage(), QStringLiteral("Линейность"));
    m_sweepPage = makeSweepPage();
    m_calTabs->addTab(makeDirectPage(), QStringLiteral("Прямая LUT"));
    m_calTabs->addTab(makeInversePage(), QStringLiteral("Обратная LUT"));
    m_calTabs->addTab(makeValidationPage(), QStringLiteral("Валидация"));

    // Внизу остаётся только окно с графиками.
    root->addWidget(makeGraphsPage(), 1);

    // UI-303 / пресет @1296: 1246…1346 МГц, 101 точка, IFBW 1 кГц.
    applyRunConfigDefaults(1.246e9, 1.346e9, 101, 1000, -30.0, 8);
    setStandCheck(false, false, false, QStringLiteral("не измерено"), false, false, -30.0, false);
    setSeriesArtifacts(QStringLiteral("—"), QStringLiteral("direct-lut.parquet"),
                       QStringLiteral("inverse-lut.parquet"), QStringLiteral("report.pdf"),
                       QStringLiteral("manifest.sha256"));
    setRunStateGuide(static_cast<int>(afar::RunState::Idle));
}

QWidget* MeasureTab::makeCalPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 2. Калибровка ВАЦ из AFAR"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));

    auto* transportNote = new QLabel(
        QStringLiteral(
            "SCPI через S2VNA Socket; отдельный UI S2VNA для "
            "калибровки не требуется. На имитаторе шаги проходят без стендовой "
            "метрологии. Перед каждым шагом — подтверждение оператора."),
        page);
    transportNote->setWordWrap(true);
    transportNote->setObjectName(QStringLiteral("hintLabel"));

    auto* kitRow = new QHBoxLayout();
    kitRow->addWidget(new QLabel(QStringLiteral("Комплект мер S2VNA:"), page));
    m_calKit = new QSpinBox(page);
    m_calKit->setObjectName(QStringLiteral("vnaCalibrationKit"));
    m_calKit->setRange(1, 64);
    m_calKit->setValue(28);
    m_calKit->setToolTip(QStringLiteral("В установленной S2VNA комплект 6650F27 соответствует №28; после выбора проверьте ответ прибора."));
    auto* applyKit = new QPushButton(QStringLiteral("Выбрать комплект"), page);
    applyKit->setObjectName(QStringLiteral("selectVnaCalibrationKit"));
    kitRow->addWidget(m_calKit);
    kitRow->addWidget(applyKit);
    kitRow->addWidget(new QLabel(QStringLiteral("6650F27-F: №28"), page));
    kitRow->addStretch(1);
    connect(applyKit, &QPushButton::clicked, this, [this] {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Комплект мер VNA"),
            QStringLiteral("Выбрать комплект мер S2VNA №%1?").arg(m_calKit->value()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes) {
            emit selectCalibrationKitRequested(m_calKit->value());
        }
    });
    m_calKitStatus = new QLabel(QStringLiteral("Комплект в S2VNA: не считан"), page);
    m_calKitStatus->setObjectName(QStringLiteral("vnaCalibrationKitStatus"));
    m_calKitStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);

    auto* kindRow = new QHBoxLayout();
    auto* kindLabel = new QLabel(QStringLiteral("Тип калибровки:"), page);
    m_calKind = new QComboBox(page);
    m_calKind->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_calKind->setMinimumContentsLength(18);
    m_calKind->addItem(QStringLiteral("1-портовая калибровка (OSL)"), 0);
    m_calKind->addItem(QStringLiteral("2-портовая калибровка (SOLT)"), 1);
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

    auto* openRow = new QHBoxLayout();
    openRow->addWidget(new QLabel(QStringLiteral("Меры порта 1:"), page));
    m_calOpenPort1 = new QComboBox(page);
    m_calOpenPort1->setObjectName(QStringLiteral("calOpenPort1"));
    m_calOpenPort1->addItem(QStringLiteral("OPEN-M (подкласс 1)"), 1);
    m_calOpenPort1->addItem(QStringLiteral("OPEN-F (подкласс 2)"), 2);
    m_calOpenPort1->setCurrentIndex(1);
    openRow->addWidget(m_calOpenPort1);
    openRow->addWidget(new QLabel(QStringLiteral("Меры порта 2:"), page));
    m_calOpenPort2 = new QComboBox(page);
    m_calOpenPort2->setObjectName(QStringLiteral("calOpenPort2"));
    m_calOpenPort2->addItem(QStringLiteral("OPEN-M (подкласс 1)"), 1);
    m_calOpenPort2->addItem(QStringLiteral("OPEN-F (подкласс 2)"), 2);
    m_calOpenPort2->setCurrentIndex(1);
    openRow->addWidget(m_calOpenPort2);
    openRow->addStretch(1);
    auto* subclassNote = new QLabel(
        QStringLiteral("Для 6650F27: OPEN-M = подкласс 1, OPEN-F = подкласс 2. "
                       "Выбор применяется также к SHORT и LOAD соответствующего порта."), page);
    subclassNote->setWordWrap(true);
    subclassNote->setObjectName(QStringLiteral("hintLabel"));

    m_calSteps = new QLabel(page);
    m_calSteps->setObjectName(QStringLiteral("calibrationSteps"));
    m_calSteps->setTextFormat(Qt::PlainText);
    m_calSteps->setWordWrap(true);
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

    // Заголовок и пояснение — подсказкой: карточка «Калибровка ВАЦ» уже подписана.
    title->hide();
    transportNote->hide();
    page->setToolTip(transportNote->text());
    layout->addLayout(kitRow);
    layout->addWidget(m_calKitStatus);
    layout->addLayout(kindRow);
    layout->addWidget(m_calKindHint);
    layout->addLayout(portRow);
    layout->addLayout(openRow);
    layout->addWidget(subclassNote);
    layout->addWidget(m_calSteps);
    layout->addWidget(m_soltStepLabel);
    layout->addWidget(m_soltHint);
    layout->addLayout(btnRow);
    layout->addWidget(m_calExtraGroup);
    m_calText->hide();  // сводка мастера запуска больше не нужна: мастера нет
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

    m_frequencyEntryMode = new QComboBox(params);
    m_frequencyEntryMode->setObjectName(QStringLiteral("frequencyEntryMode"));
    m_frequencyEntryMode->addItem(QStringLiteral("Начальная / конечная"));
    m_frequencyEntryMode->addItem(QStringLiteral("Центральная / полоса"));
    form->addRow(QStringLiteral("Задание диапазона"), m_frequencyEntryMode);

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

    m_fStartRow = makeFreqRow(&m_fStart, &m_fStartUnit);
    m_fStopRow = makeFreqRow(&m_fStop, &m_fStopUnit);
    m_fCenterRow = makeFreqRow(&m_fCenter, &m_fCenterUnit);
    m_fSpanRow = makeFreqRow(&m_fSpan, &m_fSpanUnit);
    m_fStart->setObjectName(QStringLiteral("frequencyStart"));
    m_fStop->setObjectName(QStringLiteral("frequencyStop"));
    m_fCenter->setObjectName(QStringLiteral("frequencyCenter"));
    m_fSpan->setObjectName(QStringLiteral("frequencySpan"));

    m_points = new QSpinBox(params);
    m_points->setRange(static_cast<int>(afar::c2220::kPointsMin),
                       static_cast<int>(afar::c2220::kPointsMax));

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
    m_power->setRange(afar::c2220::kPowerMinDbm, afar::c2220::kPowerMaxDbm);
    m_power->setDecimals(1);
    m_power->setSuffix(QStringLiteral(" дБм"));
    m_averages = new QSpinBox(params);
    m_averages->setObjectName(QStringLiteral("sweepAverages"));
    m_averages->setRange(afar::c2220::kAveragesMin, afar::c2220::kAveragesMax);
    m_averages->setValue(1);
    m_averages->setToolTip(QStringLiteral("S2VNA: SENS:AVER / SENS:AVER:COUN, диапазон 1…999"));

    form->addRow(QStringLiteral("f нач."), m_fStartRow);
    form->addRow(QStringLiteral("f кон."), m_fStopRow);
    form->addRow(QStringLiteral("f центр."), m_fCenterRow);
    form->addRow(QStringLiteral("Полоса"), m_fSpanRow);
    form->addRow(QStringLiteral("Точки"), m_points);
    form->addRow(QStringLiteral("ПЧ (IFBW)"), ifbwRow);
    form->addRow(QStringLiteral("Мощность"), m_power);
    form->addRow(QStringLiteral("Усреднение"), m_averages);
    auto* applyNativeSweep = new QPushButton(QStringLiteral("Применить параметры к S2VNA"), params);
    applyNativeSweep->setObjectName(QStringLiteral("applyNativeSweepSettings"));
    applyNativeSweep->setToolTip(QStringLiteral(
        "Изменяет свип прибора. После изменения диапазона или точек проверьте коррекцию VNA."));
    form->addRow(QString(), applyNativeSweep);
    connect(applyNativeSweep, &QPushButton::clicked,
            this, &MeasureTab::nativeSweepSettingsApplyRequested);
    m_nativeRfOutput = new QCheckBox(QStringLiteral("ВЧ-выход включён"), params);
    m_nativeRfOutput->setObjectName(QStringLiteral("nativeRfOutput"));
    m_nativeCorrection = new QCheckBox(QStringLiteral("Коррекция включена"), params);
    m_nativeCorrection->setObjectName(QStringLiteral("nativeCorrection"));
    auto* applyOperating = new QPushButton(QStringLiteral("Применить ВЧ и коррекцию"), params);
    applyOperating->setObjectName(QStringLiteral("applyNativeOperatingState"));
    form->addRow(m_nativeRfOutput);
    form->addRow(m_nativeCorrection);
    form->addRow(QString(), applyOperating);
    connect(applyOperating, &QPushButton::clicked, this, [this] {
        emit nativeOperatingStateApplyRequested(m_nativeRfOutput->isChecked(),
                                               m_nativeCorrection->isChecked());
    });

    connect(m_frequencyEntryMode, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onFrequencyEntryModeChanged);

    connect(m_fStart, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &MeasureTab::onFreqSpinChanged);
    connect(m_fStop, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &MeasureTab::onFreqSpinChanged);
    connect(m_fStartUnit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onFreqUnitChanged);
    connect(m_fStopUnit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onFreqUnitChanged);
    connect(m_fCenter, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &MeasureTab::onCenterSpanSpinChanged);
    connect(m_fSpan, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            &MeasureTab::onCenterSpanSpinChanged);
    connect(m_fCenterUnit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onCenterSpanUnitChanged);
    connect(m_fSpanUnit, qOverload<int>(&QComboBox::currentIndexChanged), this,
            &MeasureTab::onCenterSpanUnitChanged);
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
    onFrequencyEntryModeChanged(0);

    m_plotState = new QLabel(QStringLiteral("S-параметры: состояние ещё не выбрано"), page);
    m_plotState->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700;"));
    m_current = new QLabel(QStringLiteral("Канал: —  Att: —  Фаза: —"), page);

    m_nextHint = new QLabel(page);
    m_nextHint->setWordWrap(true);
    m_nextHint->setObjectName(QStringLiteral("hintLabel"));

    m_progress = new QProgressBar(page);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_counter = new QLabel(QStringLiteral("Состояния: 0 / 0"), page);
    m_eta = new QLabel(QStringLiteral("ETA: —"), page);

    center->addWidget(params);
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

    m_graphStack = new QStackedWidget(page);
    m_graphStack->setObjectName(QStringLiteral("graphViewStack"));
    auto* graphHint = new QLabel(
        QStringLiteral("Запустите S2VNA и нажмите «Показать график S2VNA»."), m_graphStack);
    graphHint->setAlignment(Qt::AlignCenter);
    graphHint->setWordWrap(true);
    m_graphStack->addWidget(graphHint);
    m_s2vnaGraphHost = new S2VnaGraphHost(m_graphStack);
    m_graphStack->addWidget(m_s2vnaGraphHost);
    connect(m_s2vnaGraphHost, &S2VnaGraphHost::windowLost,
            this, &MeasureTab::stopEmbeddedGraph);

    auto* side = new QWidget(page);
    side->setMinimumWidth(250);
    auto* sideLayout = new QVBoxLayout(side);
    m_graphSideLayout = sideLayout;
    sideLayout->setContentsMargins(8, 0, 0, 0);
    sideLayout->setSpacing(10);

    m_measureNow = new QPushButton(QStringLiteral("Снять измерение для отчёта"), side);
    m_measureNow->setObjectName(QStringLiteral("captureForReport"));
    m_measureNow->setToolTip(QStringLiteral(
        "Разовый двухпортовый снимок для сохранения .s2p и PDF; график S2VNA обновляется сам"));
    connect(m_measureNow, &QPushButton::clicked,
            this, &MeasureTab::captureForReportRequested);

    m_nativeGraphButton = new QPushButton(QStringLiteral("Показать график S2VNA"), side);
    m_nativeGraphButton->setObjectName(QStringLiteral("showS2vnaGraph"));
    m_nativeGraphButton->setToolTip(QStringLiteral(
        "Показать область графиков запущенной S2VNA внутри программы"));
    connect(m_nativeGraphButton, &QPushButton::clicked, this, [this] {
        QString error;
        if (!m_s2vnaGraphHost->attach(&error)) {
            QMessageBox::warning(this, QStringLiteral("График S2VNA"), error);
            return;
        }
        m_graphStack->setCurrentWidget(m_s2vnaGraphHost);
        m_nativeGraphButton->setText(QStringLiteral("График S2VNA подключён"));
        emit nativeGraphShown();
    });
    m_nativeStateButton = new QPushButton(QStringLiteral("Загрузить состояние S2VNA (.sta)"),
                                          side);
    m_nativeStateButton->setObjectName(QStringLiteral("loadS2vnaState"));
    m_nativeStateButton->setToolTip(QStringLiteral(
        "Восстановление меняет частоты, точки, мощность, ВЧ-выход, графики и может заменить калибровку S2VNA."));
    connect(m_nativeStateButton, &QPushButton::clicked,
            this, &MeasureTab::nativeStateFileRequested);
    m_nativeStateUndoButton = new QPushButton(QStringLiteral("Откатить загрузку состояния"), side);
    m_nativeStateUndoButton->setObjectName(QStringLiteral("undoS2vnaState"));
    m_nativeStateUndoButton->setEnabled(false);
    m_nativeStateUndoButton->setToolTip(QStringLiteral(
        "Вернуть S2VNA к настройкам и калибровке перед последней загрузкой .sta."));
    connect(m_nativeStateUndoButton, &QPushButton::clicked,
            this, &MeasureTab::nativeStateUndoRequested);

    m_exportTwoPort = new QPushButton(QStringLiteral("Сохранить .s2p + PDF-отчёт"), side);
    m_exportTwoPort->setObjectName(QStringLiteral("exportTwoPortReport"));
    m_exportTwoPort->setEnabled(false);
    m_exportTwoPort->setToolTip(
        QStringLiteral("Сохранить последний полный снимок без повторного измерения"));
    connect(m_exportTwoPort, &QPushButton::clicked, this, &MeasureTab::exportTwoPortRequested);

    auto* openSweepSettings = new QPushButton(
        QStringLiteral("Настройки частоты, точек и ПЧ"), side);
    connect(openSweepSettings, &QPushButton::clicked, this,
            &MeasureTab::sweepSettingsRequested);

    m_graphDataStatus = new QLabel(
        QStringLiteral("Графики обновляются в S2VNA; для .s2p и PDF нужен отдельный снимок."),
        side);
    m_graphDataStatus->setWordWrap(true);
    m_graphDataStatus->setObjectName(QStringLiteral("hintLabel"));

    auto* formatBox = new QGroupBox(QStringLiteral("Формат отображения"), side);
    auto* formatLayout = new QVBoxLayout(formatBox);
    m_graphDisplayFormat = new QComboBox(formatBox);
    m_graphDisplayFormat->setObjectName(QStringLiteral("graphDisplayFormat"));
    m_graphDisplayFormat->addItem(QStringLiteral("Амплитуда, дБ"), QStringLiteral("MLOG"));
    m_graphDisplayFormat->addItem(QStringLiteral("Амплитуда, линейная"), QStringLiteral("MLIN"));
    m_graphDisplayFormat->addItem(QStringLiteral("Фаза, °"), QStringLiteral("PHAS"));
    m_graphDisplayFormat->addItem(QStringLiteral("Групповая задержка"), QStringLiteral("GDEL"));
    m_graphDisplayFormat->setPlaceholderText(QStringLiteral("Разные форматы — выберите общий"));
    connect(m_graphDisplayFormat, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { refreshGraphsPage(true); });
    formatLayout->addWidget(m_graphDisplayFormat);

    auto* traces = new QGroupBox(QStringLiteral("Показать графики"), side);
    auto* tracesLayout = new QGridLayout(traces);
    for (int i = 0; i < static_cast<int>(m_graphTraceButtons.size()); ++i) {
        auto* button = new QPushButton(kGraphNames[i], traces);
        button->setObjectName(QStringLiteral("graphTrace%1").arg(kGraphNames[i]));
        button->setCheckable(true);
        button->setChecked(i < 4); // После измерения сразу видны все четыре S-параметра.
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
            if (!checked && std::none_of(m_graphTraceButtons.cbegin(), m_graphTraceButtons.cend(),
                                         [](const QPushButton* current) {
                                             return current && current->isChecked();
                                         })) {
                const QSignalBlocker blocker(m_graphTraceButtons[i]);
                m_graphTraceButtons[i]->setChecked(true);
            }
            refreshGraphsPage();
        });
        tracesLayout->addWidget(button, i / 2, i % 2);
        m_graphTraceButtons[i] = button;
    }
    m_graphMode = new QPushButton(QStringLiteral("Отдельно"), traces);
    m_graphMode->setObjectName(QStringLiteral("nativeTogetherButton"));
    m_graphMode->setToolTip(QStringLiteral("Переключить наложение и отдельные окна"));
    connect(m_graphMode, &QPushButton::clicked, this, [this] {
        m_graphSeparate = !m_graphSeparate;
        refreshGraphsPage();
    });
    tracesLayout->addWidget(m_graphMode, 3, 0, 1, 2);
    auto* resetScale = new QPushButton(QStringLiteral("Показать весь диапазон"), traces);
    connect(resetScale, &QPushButton::clicked, this, &MeasureTab::nativeAutoScaleRequested);
    tracesLayout->addWidget(resetScale, 4, 0, 1, 2);

    auto* markers = new QGroupBox(QStringLiteral("Маркеры"), side);
    auto* markersLayout = new QGridLayout(markers);
    m_graphMarkerMode = new QPushButton(QStringLiteral("Добавить маркер"), markers);
    connect(m_graphMarkerMode, &QPushButton::clicked, this,
            [this] { emit nativeMarkerCountChangeRequested(1); });
    auto* removeMarker = new QPushButton(QStringLiteral("Удалить последний"), markers);
    auto* clearMarkers = new QPushButton(QStringLiteral("Очистить"), markers);
    auto* refreshMarkers = new QPushButton(QStringLiteral("Обновить значения"), markers);
    connect(removeMarker, &QPushButton::clicked, this,
            [this] { emit nativeMarkerCountChangeRequested(-1); });
    connect(clearMarkers, &QPushButton::clicked, this,
            [this] { emit nativeMarkerCountChangeRequested(0); });
    connect(refreshMarkers, &QPushButton::clicked,
            this, &MeasureTab::nativeMarkerRefreshRequested);
    markersLayout->addWidget(m_graphMarkerMode, 0, 0, 1, 2);
    markersLayout->addWidget(removeMarker, 1, 0);
    markersLayout->addWidget(clearMarkers, 1, 1);
    markersLayout->addWidget(refreshMarkers, 2, 0, 1, 2);

    auto* terminalTitle = new QLabel(
        QStringLiteral("Значения маркеров и калькулятор"), side);
    terminalTitle->setStyleSheet(QStringLiteral("font-weight: 700;"));
    m_graphTerminal = new QPlainTextEdit(side);
    m_graphTerminal->setReadOnly(true);
    m_graphTerminal->setMinimumHeight(220);
    m_graphTerminal->setObjectName(QStringLiteral("graphMarkerTerminal"));
    m_graphExpression = new QLineEdit(side);
    m_graphExpression->setObjectName(QStringLiteral("graphCalculatorExpression"));
    m_graphExpression->setPlaceholderText(QStringLiteral("M2.S21.DB - M1.S21.DB"));
    auto* calculate = new QPushButton(QStringLiteral("Вычислить"), side);
    connect(calculate, &QPushButton::clicked, this, &MeasureTab::calculateGraphExpression);
    connect(m_graphExpression, &QLineEdit::returnPressed,
            this, &MeasureTab::calculateGraphExpression);

    auto* calcHint = new QLabel(
        QStringLiteral("Примеры: M1.FREQ, M1.S21.DB, M1.S21.PHASE, M1.VSWR1. "
                       "Доступны значения текущего формата трассы S2VNA."), side);
    calcHint->setWordWrap(true);
    calcHint->setObjectName(QStringLiteral("hintLabel"));

    auto* actionBox = new QGroupBox(QStringLiteral("Измерение"), side);
    actionBox->setObjectName(QStringLiteral("graphActionBox"));
    auto* actionLayout = new QVBoxLayout(actionBox);
    actionLayout->setSpacing(8);
    actionLayout->addWidget(m_measureNow);
    actionLayout->addWidget(m_nativeGraphButton);
    actionLayout->addWidget(m_nativeStateButton);
    actionLayout->addWidget(m_nativeStateUndoButton);
    actionLayout->addWidget(m_exportTwoPort);
    actionLayout->addWidget(openSweepSettings);
    actionLayout->addWidget(m_graphDataStatus);
    sideLayout->addWidget(actionBox);
    sideLayout->addWidget(formatBox);
    sideLayout->addWidget(traces);
    sideLayout->addWidget(markers);
    auto* calcBox = new QGroupBox(QStringLiteral("Значения маркеров и калькулятор"), side);
    calcBox->setObjectName(QStringLiteral("graphCalcBox"));
    auto* calcLayout = new QVBoxLayout(calcBox);
    calcLayout->setSpacing(8);
    terminalTitle->hide();
    calcLayout->addWidget(m_graphTerminal, 1);
    calcLayout->addWidget(m_graphExpression);
    calcLayout->addWidget(calculate);
    calcLayout->addWidget(calcHint);
    sideLayout->addWidget(calcBox, 1);

    auto* sideScroll = new QScrollArea(page);
    sideScroll->setObjectName(QStringLiteral("graphControlsScroll"));
    sideScroll->setWidgetResizable(true);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sideScroll->setMinimumWidth(360);
    sideScroll->setMaximumWidth(420);
    sideScroll->setWidget(side);

    // Панель управления всегда остаётся справа от графиков в одной рабочей области.
    root->addWidget(m_graphStack, 1);
    root->addWidget(sideScroll);
    m_graphMode->setEnabled(true);
    refreshMarkerTerminal();
    return page;
}

void MeasureTab::setSingleChannelControls(QWidget* controls)
{
    m_graphSideLayout->insertWidget(1, controls);
}

void MeasureTab::stopEmbeddedGraph()
{
    if (!m_s2vnaGraphHost) return;
    const bool wasActive = m_graphStack->currentWidget() == m_s2vnaGraphHost;
    m_s2vnaGraphHost->detach();
    m_graphStack->setCurrentIndex(0);
    m_nativeGraphButton->setText(QStringLiteral("Показать график S2VNA"));
    if (wasActive) emit nativeGraphHidden();
}

void MeasureTab::showEmbeddedGraph()
{
    if (m_nativeGraphButton && m_graphStack->currentWidget() != m_s2vnaGraphHost) {
        m_nativeGraphButton->click();
    }
}

void MeasureTab::setNativeGraphState(const QStringList& parameters,
                                     const QStringList& formats,
                                     int layout)
{
    m_syncNativeGraphUi = true;
    m_nativeAppliedParameters = parameters;
    m_nativeAppliedFormats = formats;
    QString commonFormat;
    bool mixedFormats = false;
    for (auto* button : m_graphTraceButtons) {
        const QSignalBlocker blocker(button);
        button->setChecked(false);
    }
    for (int i = 0; i < parameters.size() && i < formats.size(); ++i) {
        const QString parameter = parameters[i].trimmed().toUpper();
        const QString format = formats[i].trimmed().toUpper();
        int buttonIndex = -1;
        if (format == QStringLiteral("SWR") && parameter == QStringLiteral("S11")) {
            buttonIndex = 4;
        } else if (format == QStringLiteral("SWR") && parameter == QStringLiteral("S22")) {
            buttonIndex = 5;
        } else {
            const auto it = std::find(kGraphNames.cbegin(), kGraphNames.cbegin() + 4,
                                      parameter);
            if (it != kGraphNames.cbegin() + 4) {
                buttonIndex = static_cast<int>(std::distance(kGraphNames.cbegin(), it));
            }
        }
        if (buttonIndex >= 0) {
            const QSignalBlocker blocker(m_graphTraceButtons[buttonIndex]);
            m_graphTraceButtons[buttonIndex]->setChecked(true);
        }
        if (format != QStringLiteral("SWR")) {
            if (commonFormat.isEmpty()) commonFormat = format;
            else if (commonFormat != format) mixedFormats = true;
        }
    }
    {
        const QSignalBlocker blocker(m_graphDisplayFormat);
        m_graphDisplayFormat->setCurrentIndex(
            mixedFormats || m_graphDisplayFormat->findData(commonFormat) < 0
                ? -1 : m_graphDisplayFormat->findData(commonFormat));
    }
    m_graphSeparate = layout != 1;
    m_syncNativeGraphUi = false;
    m_graphMode->setEnabled(parameters.size() >= 2);
    m_graphMode->setText(m_graphSeparate ? QStringLiteral("Вместе")
                                         : QStringLiteral("Отдельно"));
}

void MeasureTab::setNativeStateLoading(bool loading)
{
    m_nativeStateBusy = loading;
    m_nativeStateButton->setEnabled(!loading);
    m_nativeStateUndoButton->setEnabled(!loading && m_nativeUndoAvailable);
    m_nativeStateButton->setText(loading ? QStringLiteral("Работа с состоянием…")
                                         : QStringLiteral("Загрузить состояние S2VNA (.sta)"));
}

void MeasureTab::setNativeStateUndoAvailable(bool available)
{
    m_nativeUndoAvailable = available;
    m_nativeStateUndoButton->setEnabled(available && !m_nativeStateBusy);
}

void MeasureTab::setInstrumentSweepSettings(double fStartHz, double fStopHz, int points,
                                            int ifbwHz, double powerDbm, int averages)
{
    applyRunConfigDefaults(fStartHz, fStopHz, points, ifbwHz, powerDbm, averages);
    markSweepSettingsChanged();
}

void MeasureTab::setNativeOperatingState(bool rfOutput, bool correction)
{
    const QSignalBlocker rfBlock(m_nativeRfOutput);
    const QSignalBlocker correctionBlock(m_nativeCorrection);
    m_nativeRfOutput->setChecked(rfOutput);
    m_nativeCorrection->setChecked(correction);
}

void MeasureTab::refreshGraphsPage(bool applySelectedFormat)
{
    if (m_syncNativeGraphUi) return;
    QStringList parameters;
    QStringList formats;
    for (int i = 0; i < static_cast<int>(m_graphTraceButtons.size()); ++i) {
        if (m_graphTraceButtons[i] != nullptr && m_graphTraceButtons[i]->isChecked()) {
            parameters << (i == 4 ? QStringLiteral("S11")
                            : i == 5 ? QStringLiteral("S22") : kGraphNames[i]);
            QString format = i >= 4 ? QStringLiteral("SWR") : QString();
            if (format.isEmpty() && !applySelectedFormat) {
                const int prior = m_nativeAppliedParameters.indexOf(parameters.back());
                if (prior >= 0 && prior < m_nativeAppliedFormats.size()) {
                    format = m_nativeAppliedFormats[prior];
                }
            }
            if (format.isEmpty()) format = m_graphDisplayFormat->currentData().toString();
            if (format.isEmpty()) format = QStringLiteral("MLOG");
            formats << format;
        }
    }
    if (parameters.isEmpty()) return;
    if (parameters.size() < 2) m_graphSeparate = false;
    m_graphDisplayFormat->setEnabled(m_graphTraceButtons[0]->isChecked()
                                     || m_graphTraceButtons[1]->isChecked()
                                     || m_graphTraceButtons[2]->isChecked()
                                     || m_graphTraceButtons[3]->isChecked());
    m_graphMode->setEnabled(parameters.size() >= 2);
    m_graphMode->setText(m_graphSeparate ? QStringLiteral("Вместе")
                                         : QStringLiteral("Отдельно"));
    emit nativeGraphConfigurationRequested(parameters, formats, m_graphSeparate);
}

void MeasureTab::setNativeMarkerValues(const QStringList& lines,
                                       const QStringList& keys,
                                       const QVector<double>& values,
                                       const QVector<double>& frequenciesGhz)
{
    m_nativeMarkerLines = lines;
    m_graphMarkers = frequenciesGhz;
    m_nativeMarkerOperands.clear();
    for (int i = 0; i < keys.size() && i < values.size(); ++i) {
        m_nativeMarkerOperands.insert(keys[i].toUpper(), values[i]);
    }
    refreshMarkerTerminal();
}

void MeasureTab::refreshMarkerTerminal()
{
    QStringList lines = m_nativeMarkerLines;
    if (lines.isEmpty()) {
        lines << QStringLiteral("Добавьте маркер в S2VNA или обновите значения.");
    }
    if (!m_graphCalculations.isEmpty()) {
        lines << QString() << QStringLiteral("Расчёты:") << m_graphCalculations;
    }
    m_graphTerminal->setPlainText(lines.join(QLatin1Char('\n')));
    m_graphTerminal->verticalScrollBar()->setValue(m_graphTerminal->verticalScrollBar()->maximum());
}

double MeasureTab::graphOperandValue(const QString& token, bool* ok) const
{
    QString normalized = token.trimmed().toUpper();
    normalized.replace(QLatin1Char(','), QLatin1Char('.'));
    const double number = normalized.toDouble(ok);
    if (*ok) return number;
    const auto it = m_nativeMarkerOperands.constFind(normalized);
    *ok = it != m_nativeMarkerOperands.cend();
    return *ok ? it.value() : 0.0;
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
        const QSignalBlocker b3(m_fCenterUnit);
        const QSignalBlocker b4(m_fSpanUnit);
        m_fStartUnit->setCurrentIndex(unit);
        m_fStopUnit->setCurrentIndex(unit);
        m_fCenterUnit->setCurrentIndex(unit);
        m_fSpanUnit->setCurrentIndex(unit);
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
    // Настройки свипа теперь живут в верхней панели и не открываются автоматически при смене этапа.
    if (stageIndex0 == 3) {
        return;
    }
    const int tab = stageIndex0 > 3 ? stageIndex0 - 2 : stageIndex0 - 1;
    if (tab >= 0 && tab < m_calTabs->count()) {
        m_calTabs->setCurrentIndex(tab);
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
}

void MeasureTab::setVnaCalibrationIdHint(const QString& id)
{
    m_vnaCalId = id.trimmed();
    const auto match = QRegularExpression(QStringLiteral("^CKIT\\s+(\\d+)"))
                           .match(m_vnaCalId);
    if (match.hasMatch()) {
        const int index = match.captured(1).toInt();
        if (index >= m_calKit->minimum() && index <= m_calKit->maximum()) {
            const QSignalBlocker blocker(m_calKit);
            m_calKit->setValue(index);
        }
    }
    if (m_calKitStatus) {
        m_calKitStatus->setText(m_vnaCalId.isEmpty()
                                    ? QStringLiteral("Комплект в S2VNA: не считан")
                                    : QStringLiteral("Комплект в S2VNA: %1").arg(m_vnaCalId));
    }
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
    // Все шаги выбранной калибровки: ✓ выполнен, ▶ текущий, ○ впереди.
    QStringList lines;
    for (int step = 0; step <= apply; ++step) {
        const QString mark = step < m_soltStep ? QStringLiteral("✓")
                             : step == m_soltStep ? QStringLiteral("▶") : QStringLiteral("○");
        lines << QStringLiteral("%1  %2").arg(mark, calStepTitle(step));
    }
    if (m_calSteps) {
        m_calSteps->setText(lines.join(QLatin1Char('\n')));
    }
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
    const int kind = isOnePortCal() ? 0 : 1;
    const int port = m_calPort ? m_calPort->currentData().toInt() : 1;
    const int standardPort = kind == 0 ? port : (m_soltStep >= 4 && m_soltStep <= 6 ? 2 : 1);
    const auto* selectedOpen = standardPort == 1 ? m_calOpenPort1 : m_calOpenPort2;
    const int subclass = selectedOpen->currentData().toInt();
    const bool isStandard = kind == 0 ? m_soltStep >= 1 && m_soltStep <= 3
                                      : m_soltStep >= 1 && m_soltStep <= 6;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Подтверждение шага калибровки"),
        QStringLiteral("Выполнить шаг:\n%1%2?")
            .arg(calStepTitle(m_soltStep),
                 isStandard ? QStringLiteral("\nВыбрано для порта %1: %2")
                                  .arg(standardPort).arg(selectedOpen->currentText())
                            : QString()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    m_soltBusy = true;
    refreshSoltUi();
    emit calibrateStepRequested(kind, m_soltStep, port, subclass);
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

void MeasureTab::setTwoPortExportEnabled(bool enabled)
{
    m_twoPortExportAvailable = enabled;
    if (m_exportTwoPort) {
        m_exportTwoPort->setEnabled(enabled && !m_twoPortExportInProgress);
    }
}

void MeasureTab::setTwoPortExportInProgress(bool inProgress)
{
    m_twoPortExportInProgress = inProgress;
    if (m_exportTwoPort) {
        m_exportTwoPort->setEnabled(m_twoPortExportAvailable && !inProgress);
        m_exportTwoPort->setText(inProgress
                                     ? QStringLiteral("Формирование отчёта…")
                                     : QStringLiteral("Сохранить .s2p + PDF-отчёт"));
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

QString MeasureTab::reportDeviceName() const
{
    return m_reportDeviceName ? m_reportDeviceName->text().trimmed() : QString{};
}

QString MeasureTab::reportDeviceSerial() const
{
    return m_reportDeviceSerial ? m_reportDeviceSerial->text().trimmed() : QString{};
}

QString MeasureTab::reportOperatorName() const
{
    return m_reportOperator ? m_reportOperator->text().trimmed() : QString{};
}

QString MeasureTab::reportComment() const
{
    return m_reportComment ? m_reportComment->text().trimmed() : QString{};
}

bool MeasureTab::reportAccepted() const
{
    return m_reportAccepted && m_reportAccepted->isChecked();
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
    // Пределы C2220 в выбранных единицах.
    const double scale = freqUnitScale(unitIndex);
    const double minHz = static_cast<double>(afar::c2220::kFrequencyMinHz);
    const double maxHz = static_cast<double>(afar::c2220::kFrequencyMaxHz);
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
    applyFreqSpinLimits(m_fCenter, m_fCenterUnit->currentIndex());
    const double spanScale = freqUnitScale(m_fSpanUnit->currentIndex());
    m_fSpan->setRange(1.0 / spanScale,
                      static_cast<double>(afar::c2220::kFrequencyMaxHz
                                          - afar::c2220::kFrequencyMinHz) / spanScale);
    m_fSpan->setDecimals(m_fSpanUnit->currentIndex() == 0 ? 0
                         : (m_fSpanUnit->currentIndex() == 3 ? 6 : 3));
    m_fStart->setValue(m_fStartHz / freqUnitScale(m_fStartUnit->currentIndex()));
    m_fStop->setValue(m_fStopHz / freqUnitScale(m_fStopUnit->currentIndex()));
    const double centerHz = (m_fStartHz + m_fStopHz) / 2.0;
    const double spanHz = std::max(1.0, m_fStopHz - m_fStartHz);
    m_fCenter->setValue(centerHz / freqUnitScale(m_fCenterUnit->currentIndex()));
    m_fSpan->setValue(spanHz / spanScale);
    m_freqUiGuard = false;
}

void MeasureTab::syncIfbwSpinFromHz()
{
    m_freqUiGuard = true;
    const double scale = ifbwUnitScale(m_ifbwUnit->currentIndex());
    m_ifbw->setRange(static_cast<double>(afar::c2220::kIfbwMinHz) / scale,
                     static_cast<double>(afar::c2220::kIfbwMaxHz) / scale);
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
    syncFreqSpinsFromHz();
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

void MeasureTab::onCenterSpanSpinChanged()
{
    if (m_freqUiGuard) {
        return;
    }
    const double centerHz = m_fCenter->value()
        * freqUnitScale(m_fCenterUnit->currentIndex());
    const double spanHz = m_fSpan->value() * freqUnitScale(m_fSpanUnit->currentIndex());
    constexpr double kMinimumHz = static_cast<double>(afar::c2220::kFrequencyMinHz);
    constexpr double kMaximumHz = static_cast<double>(afar::c2220::kFrequencyMaxHz);
    m_fStartHz = std::max(kMinimumHz, centerHz - spanHz / 2.0);
    m_fStopHz = std::min(kMaximumHz, centerHz + spanHz / 2.0);
    if (m_fStopHz <= m_fStartHz) {
        m_fStopHz = std::min(kMaximumHz, m_fStartHz + 1.0);
    }
    syncFreqSpinsFromHz();
    markSweepSettingsChanged();
}

void MeasureTab::onCenterSpanUnitChanged()
{
    if (m_freqUiGuard) {
        return;
    }
    syncFreqSpinsFromHz();
}

void MeasureTab::onFrequencyEntryModeChanged(int index)
{
    if (m_fStartRow == nullptr) {
        return;
    }
    auto* form = qobject_cast<QFormLayout*>(m_fStartRow->parentWidget()->layout());
    if (form == nullptr) {
        return;
    }
    const bool centerSpan = index == 1;
    form->setRowVisible(m_fStartRow, !centerSpan);
    form->setRowVisible(m_fStopRow, !centerSpan);
    form->setRowVisible(m_fCenterRow, centerSpan);
    form->setRowVisible(m_fSpanRow, centerSpan);
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
    QSettings settings;
    settings.setValue(QStringLiteral("sweep/f_start_hz"), m_fStartHz);
    settings.setValue(QStringLiteral("sweep/f_stop_hz"), m_fStopHz);
    settings.setValue(QStringLiteral("sweep/points"), m_points->value());
    settings.setValue(QStringLiteral("sweep/ifbw_hz"), m_ifbwHz);
    settings.setValue(QStringLiteral("sweep/power_dbm"), m_power->value());
    settings.setValue(QStringLiteral("sweep/averages"), m_averages->value());

    const QString text = QStringLiteral(
        "Настройки изменены. Последний график сохранён; "
        "при построении новые параметры применятся в следующем цикле.");
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
