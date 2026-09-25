#include "MeasureTab.h"

#include "S21PlotWidget.h"

#include "../measure/RunStateMachine.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <cmath>

MeasureTab::MeasureTab(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_stages = new QListWidget(this);
    const QStringList stages = {
        QStringLiteral("1 Подключения"),
        QStringLiteral("2 Калибровка ВАЦ (S2VNA)"),
        QStringLiteral("3 Линейность"),
        QStringLiteral("4 Перебор кодов"),
        QStringLiteral("5 Прямая LUT"),
        QStringLiteral("6 Обратная LUT"),
        QStringLiteral("7 Валидация"),
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
    auto* title = new QLabel(QStringLiteral("Этап 2. Калибровка ВАЦ (в S2VNA)"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_calText = new QLabel(page);
    m_calText->setWordWrap(true);
    m_calText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    layout->addWidget(m_calText);
    layout->addStretch(1);
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

    m_plotState = new QLabel(QStringLiteral("S-параметры: состояние ещё не выбрано"), page);
    m_plotState->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700;"));
    m_current = new QLabel(QStringLiteral("Канал: —  Att: —  Фаза: —"), page);

    m_plotLegend = new QLabel(
        QStringLiteral("Четыре трассы текущего состояния (не LUT)."), page);
    m_plotLegend->setWordWrap(true);
    m_plotLegend->setObjectName(QStringLiteral("hintLabel"));

    m_nextHint = new QLabel(page);
    m_nextHint->setWordWrap(true);
    m_nextHint->setObjectName(QStringLiteral("hintLabel"));

    auto* plotsWrap = new QWidget(page);
    auto* grid = new QGridLayout(plotsWrap);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(4);

    auto makePlot = [plotsWrap](const QString& magTitle, const QString& phaseTitle) {
        auto* w = new S21PlotWidget(plotsWrap);
        w->setMinimumHeight(110);
        w->setPanelTitles(magTitle, phaseTitle);
        w->setEmptyHint(
            QStringLiteral("Нет данных свипа — нажмите Старт или выберите ячейку матрицы"));
        return w;
    };
    m_plotS11 = makePlot(QStringLiteral("|S11| (дБ)"), QStringLiteral("фаза unwrap (°)"));
    m_plotS21 = makePlot(QStringLiteral("|S21| (дБ)"), QStringLiteral("фаза unwrap (°)"));
    m_plotS12 = makePlot(QStringLiteral("|S12| (дБ)"), QStringLiteral("фаза unwrap (°)"));
    m_plotS22 = makePlot(QStringLiteral("|S22| (дБ)"), QStringLiteral("фаза unwrap (°)"));
    m_plotS11->setSubtitle(QStringLiteral("S11"));
    m_plotS21->setSubtitle(QStringLiteral("S21"));
    m_plotS12->setSubtitle(QStringLiteral("S12"));
    m_plotS22->setSubtitle(QStringLiteral("S22"));

    grid->addWidget(m_plotS11, 0, 0);
    grid->addWidget(m_plotS21, 0, 1);
    grid->addWidget(m_plotS12, 1, 0);
    grid->addWidget(m_plotS22, 1, 1);
    grid->setRowStretch(0, 1);
    grid->setRowStretch(1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    m_progress = new QProgressBar(page);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_counter = new QLabel(QStringLiteral("Состояния: 0 / 0"), page);
    m_eta = new QLabel(QStringLiteral("ETA: —"), page);

    center->addWidget(params);
    center->addWidget(m_plotState);
    center->addWidget(m_current);
    center->addWidget(m_plotLegend);
    center->addWidget(m_nextHint);
    center->addWidget(plotsWrap, 1);
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
    m_plotS21->setOverlayCurves({}, {});
    m_plotS21->setCurves(freqGhz, magDb, phaseUnwrapDeg);
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
    m_plotS11->setOverlayCurves({}, {});
    m_plotS21->setOverlayCurves({}, {});
    m_plotS12->setOverlayCurves({}, {});
    m_plotS22->setOverlayCurves({}, {});
    m_plotS11->setCurves(freqGhz, s11mag, s11ph);
    m_plotS21->setCurves(freqGhz, s21mag, s21ph);
    m_plotS12->setCurves(freqGhz, s12mag, s12ph);
    m_plotS22->setCurves(freqGhz, s22mag, s22ph);
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
            "Полная калибровка ВАЦ (SOLT / Response / Thru) выполняется в программе "
            "S2VNA до серии. Здесь — только подтверждение оператора и пороги FR-05 "
            "как настройки ПО. См. docs/S2VNA-setup.md. AFAR не шлёт SCPI калибровки.\n\n"
            "Подключения: %1\n"
            "IDN C2220 / контроллер: %2\n"
            "Калибровка ВАЦ: %3\n"
            "THRU: %4\n"
            "%5\n\n"
            "Пороги FR-05 по умолчанию: 0,20 дБ / 2,0° — в мастере.")
            .arg(m_connectionsOk ? QStringLiteral("подтверждены")
                                 : QStringLiteral("ожидают мастера"),
                 m_idnOk ? QStringLiteral("подтверждены") : QStringLiteral("ожидают мастера"),
                 m_calOk ? QStringLiteral("подтверждена оператором (в S2VNA)")
                         : QStringLiteral("не подтверждена"),
                 m_thruText,
                 idLine));
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
}

void MeasureTab::onIfbwUnitChanged()
{
    if (m_freqUiGuard) {
        return;
    }
    syncIfbwSpinFromHz();
}
