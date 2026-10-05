#include "MainWindow.h"

#include "AcceptanceAtTab.h"
#include "CodeMatrixTab.h"
#include "ConnectionBar.h"
#include "DataFormatsTab.h"
#include "MeasureTab.h"
#include "MeasureWorker.h"
#include "RunConfig.h"
#include "RunStateMachine.h"
#include "StartWizard.h"
#include "afar/C2220Limits.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QDebug>
#include <QFrame>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#include <cmath>
#include <filesystem>
#include <string>

#ifndef AFAR_EXAMPLES_DIR
#define AFAR_EXAMPLES_DIR ""
#endif

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("AFAR RX Calibration Studio"));

    const QRect workArea = screen() ? screen()->availableGeometry()
                                    : QGuiApplication::primaryScreen()->availableGeometry();
    resize(qMin(1000, qMax(1, workArea.width() - 32)),
           qMin(620, qMax(1, workArea.height() - 32)));
    move(workArea.center() - rect().center());

    auto* viewport = new QScrollArea(this);
    viewport->setWidgetResizable(true);
    viewport->setFrameShape(QFrame::NoFrame);

    auto* root = new QWidget(viewport);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(4);

    m_connections = new ConnectionBar(root);
    m_connections->setVnaInfo(QStringLiteral("PLANAR C2220"), QStringLiteral("VnaSimulator"),
                              false);
    m_connections->setControllerInfo(QStringLiteral("DutSimulator"), false);
    m_connections->setRunStatus(QStringLiteral("простой"), QStringLiteral("#333333"));

    auto* tabs = new QTabWidget(root);
    m_measure = new MeasureTab(tabs);
    m_matrix = new CodeMatrixTab(tabs);
    m_formats = new DataFormatsTab(tabs);
    m_acceptance = new AcceptanceAtTab(tabs);
    tabs->addTab(m_measure, QStringLiteral("Измерение"));
    tabs->addTab(m_matrix, QStringLiteral("Матрица кодов"));
    tabs->addTab(m_formats, QStringLiteral("Форматы данных"));
    tabs->addTab(m_acceptance, QStringLiteral("Приёмка AT"));

    auto* cycle = new QFrame(root);
    cycle->setFrameShape(QFrame::StyledPanel);
    auto* cycleLayout = new QHBoxLayout(cycle);
    m_wizardBtn = new QPushButton(QStringLiteral("Мастер запуска"), cycle);
    m_wizardBtn->setObjectName(QStringLiteral("btnWizard"));
    cycle->setToolTip(
        QStringLiteral("Мастер запуска → подтверждения → Готово → Старт"));
    m_start = new QPushButton(QStringLiteral("Старт"), cycle);
    m_start->setObjectName(QStringLiteral("btnStart"));
    m_pause = new QPushButton(QStringLiteral("Пауза"), cycle);
    m_pause->setObjectName(QStringLiteral("btnPause"));
    m_stop = new QPushButton(QStringLiteral("Стоп"), cycle);
    m_stop->setObjectName(QStringLiteral("btnStop"));
    m_start->setEnabled(false);
    m_pause->setEnabled(false);
    m_stop->setEnabled(false);
    cycleLayout->addWidget(m_wizardBtn);
    cycleLayout->addStretch(1);
    cycleLayout->addWidget(m_start);
    cycleLayout->addWidget(m_pause);
    cycleLayout->addWidget(m_stop);

    rootLayout->addWidget(m_connections);
    rootLayout->addWidget(tabs, 1);
    rootLayout->addWidget(cycle);
    viewport->setWidget(root);
    setCentralWidget(viewport);

    m_operationText = new QLabel(QStringLiteral("Готово"), this);
    m_operationText->setObjectName(QStringLiteral("operationStatusText"));
    m_operationText->setMinimumWidth(300);
    m_operationProgress = new QProgressBar(this);
    m_operationProgress->setObjectName(QStringLiteral("operationProgressBar"));
    m_operationProgress->setRange(0, 100);
    m_operationProgress->setValue(100);
    m_operationProgress->setFixedWidth(220);
    statusBar()->addPermanentWidget(m_operationText, 1);
    statusBar()->addPermanentWidget(m_operationProgress);

    const auto applyWorkMode = [tabs, cycle](bool channelMode) {
        cycle->setVisible(channelMode);
        for (int tab = 1; tab < tabs->count(); ++tab) {
            tabs->setTabVisible(tab, channelMode);
        }
    };
    connect(m_measure, &MeasureTab::workModeChanged, this, applyWorkMode);
    applyWorkMode(false);

    m_thread = new QThread(this);
    m_worker = new MeasureWorker();
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_wizardBtn, &QPushButton::clicked, this, &MainWindow::onOpenWizard);
    connect(m_start, &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_pause, &QPushButton::clicked, this, &MainWindow::onPause);
    connect(m_stop, &QPushButton::clicked, this, &MainWindow::onStop);

    connect(m_worker, &MeasureWorker::prepareFinished, this, &MainWindow::onPrepareFinished);
    connect(m_worker, &MeasureWorker::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_worker, &MeasureWorker::connectionChanged, this, &MainWindow::onConnectionChanged);
    connect(m_worker, &MeasureWorker::progressChanged, this, &MainWindow::onProgress);
    connect(m_worker, &MeasureWorker::etaChanged, this, &MainWindow::onEtaChanged);
    connect(m_worker, &MeasureWorker::sweepPreview, this, &MainWindow::onSweepPreview);
    connect(m_worker, &MeasureWorker::sparamsPreview, this, &MainWindow::onSparamsPreview);
    connect(m_worker, &MeasureWorker::pathsChanged, this, &MainWindow::onPaths);
    connect(m_worker, &MeasureWorker::matrixSnapshot, this, &MainWindow::onMatrixSnapshot);
    connect(m_worker, &MeasureWorker::axesChanged, this,
            [this](const QVector<int>& ch, const QVector<int>& att) {
                m_matrix->setChannelAttChoices(ch, att);
            });
    connect(m_worker, &MeasureWorker::diagnostic, this, &MainWindow::onDiagnostic);
    connect(m_worker, &MeasureWorker::operationProgress, this,
            [this](const QString& text, int completed, int total) {
                setOperationProgress(text, completed, total);
            });
    connect(m_worker, &MeasureWorker::scpiErrorsReceived, this,
            [this](const QStringList& entries) { m_connections->appendScpiErrors(entries); });
    connect(m_matrix, &CodeMatrixTab::selectionChanged, this,
            &MainWindow::onMatrixSelectionChanged);
    connect(m_matrix, &CodeMatrixTab::remeasureRequested, this,
            &MainWindow::onRemeasureRequested);
    connect(m_matrix, &CodeMatrixTab::cellInspectRequested, this,
            &MainWindow::onCellInspectRequested);
    connect(m_worker, &MeasureWorker::cellSweepPreview, this, &MainWindow::onCellSweepPreview);
    connect(m_connections, &ConnectionBar::themeToggleRequested, this, &MainWindow::onToggleTheme);
    connect(m_connections, &ConnectionBar::vnaSettingsChanged, this, &MainWindow::onApplyVnaSettings);
    connect(m_connections, &ConnectionBar::controllerSettingsChanged, this,
            &MainWindow::onApplyControllerSettings);
    connect(m_connections, &ConnectionBar::probeVnaRequested, this, &MainWindow::onProbeVna);
    connect(m_connections, &ConnectionBar::simulateScpiErrorRequested, this, [this]() {
        QMetaObject::invokeMethod(m_worker, "simulateScpiError", Qt::QueuedConnection);
    });
    connect(m_worker, &MeasureWorker::probeFinished, this, &MainWindow::onProbeFinished);
    connect(m_worker, &MeasureWorker::vnaCalibrationDetected, this,
            [this](const QString& id, bool enabled) {
                m_measure->setVnaCalibrationIdHint(id);
                QSettings().setValue(QStringLiteral("ui/vna_calibration_id"), id);
                statusBar()->showMessage(
                    QStringLiteral("Комплект VNA: %1 · коррекция: %2")
                        .arg(id, enabled ? QStringLiteral("ВКЛ") : QStringLiteral("ВЫКЛ")),
                    12000);
            });
    connect(m_worker, &MeasureWorker::seriesArtifactsPreview, this,
            &MainWindow::onSeriesArtifactsPreview);
    connect(m_worker, &MeasureWorker::directLutCurvePreview, this,
            &MainWindow::onDirectLutCurvePreview);
    connect(m_measure, &MeasureTab::openWizardRequested, this, &MainWindow::onOpenWizard);
    connect(m_measure, &MeasureTab::resumeSeriesRequested, this, &MainWindow::onResumeSeries);
    connect(m_measure, &MeasureTab::measureNowRequested, this, &MainWindow::onMeasureNow);
    connect(m_measure, &MeasureTab::exportTwoPortRequested, this, &MainWindow::onExportTwoPort);
    connect(m_measure, &MeasureTab::connectAndMeasureRequested, this,
            &MainWindow::onConnectAndMeasureVna);
    connect(m_measure, &MeasureTab::calibrateStepRequested, this, &MainWindow::onCalibrateStep);
    connect(m_measure, &MeasureTab::selectCalibrationKitRequested, this,
            [this](int index) {
                QMetaObject::invokeMethod(m_worker, "selectCalibrationKit", Qt::QueuedConnection,
                                          Q_ARG(int, index));
            });
    connect(m_worker, &MeasureWorker::calibrateTwoPortFinished, m_measure,
            &MeasureTab::onCalibrateStepFinished, Qt::QueuedConnection);
    connect(m_worker, &MeasureWorker::calibrateOnePortFinished, m_measure,
            &MeasureTab::onCalibrateStepFinished, Qt::QueuedConnection);
    connect(m_worker, &MeasureWorker::measureNowFinished, this,
            [this](bool ok, const QString& message) {
                if (!ok) {
                    m_connections->setDiagnostic(
                        message.isEmpty() ? QStringLiteral("Измерить сейчас: отказ") : message);
                } else {
                    m_connections->setDiagnostic(QStringLiteral("Измерить сейчас: OK"));
                    m_measure->setStageHighlight(7);
                }
            });
    connect(m_worker, &MeasureWorker::twoPortExportAvailable, m_measure,
            &MeasureTab::setTwoPortExportEnabled, Qt::QueuedConnection);
    connect(m_worker, &MeasureWorker::exportTwoPortFinished, this,
            [this](bool ok, const QString& s2p, const QString& pdf, const QString& message) {
                m_measure->setTwoPortExportInProgress(false);
                if (!ok) {
                    statusBar()->showMessage(QStringLiteral("Отчёт не сохранён"), 15000);
                    QMessageBox::warning(this, QStringLiteral("Экспорт измерения"), message);
                    return;
                }
                m_connections->setDiagnostic(message);
                statusBar()->showMessage(QStringLiteral("Отчёт сохранён: %1").arg(pdf), 30000);
                QMessageBox::information(
                    this, QStringLiteral("Экспорт измерения"),
                    QStringLiteral("Файлы сохранены:\n\nS2P: %1\nPDF: %2").arg(s2p, pdf));
            });

    statusBar()->showMessage(
        QStringLiteral("ВАЦ (S2VNA): не подтверждена · THRU: — · Сырые данные: append-only · SHA-256"));

    m_thread->start();
    loadExampleDefaults();
    refreshThemeButton();
    scanUnfinishedSeries();
    onApplyVnaSettings();
    onApplyControllerSettings();
}

MainWindow::~MainWindow()
{
    if (m_thread && m_worker) {
        m_worker->interruptIo();
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::QueuedConnection);
        using afar::RunState;
        const auto state = static_cast<RunState>(m_state);
        const bool seriesActive = state == RunState::Running || state == RunState::Pausing
            || state == RunState::Paused
            || state == RunState::Stopping || state == RunState::Finalizing;
        if (!m_thread->wait(seriesActive ? 30000 : 5000)) {
            qWarning() << "Worker shutdown is taking longer than expected; waiting to avoid data corruption";
            m_thread->wait();
        }
    }
}

void MainWindow::loadExampleDefaults()
{
    const std::filesystem::path examples(AFAR_EXAMPLES_DIR);
    const auto cfg1296 = examples / "run-config.c2220-1296.example.json";
    const auto cfgDefault = examples / "run-config.example.json";
    afar::RunConfig cfg;
    std::string diag;
    auto applyCfg = [this](const afar::RunConfig& c) {
        m_measure->applyRunConfigDefaults(
            static_cast<double>(c.vna.f_start_hz), static_cast<double>(c.vna.f_stop_hz),
            c.vna.points, c.vna.ifbw_hz, c.vna.power_dbm, c.vna.averages);
        m_connections->setVnaInfo(QString::fromStdString(c.vna.model),
                                  QStringLiteral("VnaSimulator (не Socket)"), false);
        m_connections->setControllerInfo(QStringLiteral("DutSimulator"), false);
    };
    if (!examples.empty() && afar::RunConfig::loadFromFile(cfg1296, cfg, diag)) {
        applyCfg(cfg);
    } else if (!examples.empty() && afar::RunConfig::loadFromFile(cfgDefault, cfg, diag)) {
        applyCfg(cfg);
    } else {
        // Минимум UI-303: пресет @1296 МГц.
        m_measure->applyRunConfigDefaults(1.246e9, 1.346e9, 101, 1000, -30.0, 8);
    }

    QSettings saved;
    if (saved.contains(QStringLiteral("sweep/f_start_hz"))) {
        const double fStart = saved.value(QStringLiteral("sweep/f_start_hz")).toDouble();
        const double fStop = saved.value(QStringLiteral("sweep/f_stop_hz")).toDouble();
        if (std::isfinite(fStart) && std::isfinite(fStop)
            && fStart >= static_cast<double>(afar::c2220::kFrequencyMinHz)
            && fStop <= static_cast<double>(afar::c2220::kFrequencyMaxHz)
            && fStart < fStop) {
            m_measure->applyRunConfigDefaults(
                fStart, fStop,
                saved.value(QStringLiteral("sweep/points"), m_measure->points()).toInt(),
                saved.value(QStringLiteral("sweep/ifbw_hz"), m_measure->ifbwHz()).toInt(),
                saved.value(QStringLiteral("sweep/power_dbm"), m_measure->powerDbm()).toDouble(),
                saved.value(QStringLiteral("sweep/averages"), m_measure->averages()).toInt());
        }
    }
}

void MainWindow::onOpenWizard()
{
    StartWizard wizard(this);
    wizard.setSweepPreset(m_measure->fStartHz(), m_measure->fStopHz(), m_measure->points(),
                          m_measure->ifbwHz(), m_measure->powerDbm(), m_measure->averages());
    wizard.setVnaEndpoint(m_connections->vnaHost(), m_connections->vnaPort());
    connect(&wizard, &StartWizard::probeCodesRequested, this,
            [this](double fStartHz, double fStopHz, int points, int ifbwHz, double powerDbm,
                   int averages) {
                onApplyVnaSettings();
                QMetaObject::invokeMethod(m_worker, "runProbeCodes", Qt::QueuedConnection,
                                          Q_ARG(double, fStartHz), Q_ARG(double, fStopHz),
                                          Q_ARG(int, points), Q_ARG(int, ifbwHz),
                                          Q_ARG(double, powerDbm), Q_ARG(int, averages));
            });
    connect(m_worker, &MeasureWorker::probeCodesFinished, &wizard,
            &StartWizard::onProbeCodesFinished, Qt::QueuedConnection);
    if (wizard.exec() != QDialog::Accepted) {
        return;
    }
    QString diag;
    if (!wizard.materializeSimFixtures(diag)) {
        QMessageBox::critical(this, QStringLiteral("Фикстуры"), diag);
        return;
    }
    if (wizard.directAccessRequested()) {
        m_connections->setDiagnostic(QStringLiteral(
            "Запрошен direct access — только подтверждение инженера, SCPI не уходит."));
        // На имитаторе выставляем флаг без SCPI.
        // VnaSimulator живёт в worker; флаг отражается диагностикой.
    }
    m_start->setEnabled(false);
    m_pause->setEnabled(false);
    m_stop->setEnabled(false);
    m_measure->setStandCheck(wizard.connectionsConfirmed(), wizard.idnConfirmed(),
                            wizard.calConfirmed(), wizard.thruSummary(),
                            wizard.noOverloadConfirmed(), wizard.probeConfirmed(),
                            wizard.powerDbm(), wizard.engineerProfile());
    m_measure->setVnaCalibrationIdHint(wizard.vnaCalibrationId());
    statusBar()->showMessage(
        QStringLiteral("ВАЦ (S2VNA): %1 · THRU: %2 · Сырые данные: append-only · SHA-256")
            .arg(wizard.calConfirmed() ? QStringLiteral("подтверждена оператором")
                                       : QStringLiteral("не подтверждена"),
                 wizard.thruSummary()));
    QMetaObject::invokeMethod(m_worker, "prepare", Qt::QueuedConnection,
                              Q_ARG(QString, wizard.dataRoot()),
                              Q_ARG(QString, wizard.runConfigPath()),
                              Q_ARG(QString, wizard.attenuatorCsvPath()),
                              Q_ARG(bool, wizard.forceSafeState()),
                              Q_ARG(QString, wizard.vnaCalibrationId()));
}

void MainWindow::onStart()
{
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection);
}

void MainWindow::onPause()
{
    QMetaObject::invokeMethod(m_worker, "pause", Qt::QueuedConnection);
}

void MainWindow::onStop()
{
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Остановить серию"),
        QStringLiteral("Завершить текущую транзакцию, перевести стенд в безопасное "
                       "состояние и закрыть серию как ABORTED? Данные останутся "
                       "пригодными для анализа (FR-11)."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
}

void MainWindow::onPrepareFinished(bool ok, const QString& diagnostics)
{
    if (!ok) {
        m_connections->setDiagnostic(diagnostics);
        QMessageBox::warning(this, QStringLiteral("Подготовка серии"),
                             diagnostics.isEmpty()
                                 ? QStringLiteral("prepare не удался")
                                 : diagnostics);
        return;
    }
    m_connections->setDiagnostic(QString());
    m_measure->setStageHighlight(3);
    m_measure->setEtaText(QStringLiteral("ETA: —"));
    updateCycleButtons(m_state);
}

void MainWindow::onStateChanged(int state, const QString& russianText, const QString& colorName)
{
    m_state = state;
    m_connections->setRunStatus(russianText, colorName);
    updateCycleButtons(state);
    m_measure->setRunStateGuide(state);
    using afar::RunState;
    const auto st = static_cast<RunState>(state);
    const bool lockSettings = st == RunState::Running || st == RunState::Pausing
        || st == RunState::Paused || st == RunState::Stopping || st == RunState::Finalizing
        || st == RunState::Connecting || st == RunState::SelfTest || st == RunState::Ready;
    m_connections->setVnaSettingsLocked(lockSettings);
    if (st == RunState::Running) {
        m_measure->setStageHighlight(3);
    } else if (st == RunState::Complete) {
        m_measure->setStageHighlight(6);
    }
}

void MainWindow::updateCycleButtons(int state)
{
    using afar::RunState;
    const auto st = static_cast<RunState>(state);
    const bool ready = st == RunState::Ready;
    const bool running = st == RunState::Running || st == RunState::Pausing;
    const bool paused = st == RunState::Paused;
    m_start->setEnabled(ready || paused);
    m_start->setText(paused ? QStringLiteral("Продолжить") : QStringLiteral("Старт"));
    m_pause->setEnabled(running);
    m_stop->setEnabled(running || paused);
    // UI-MEAS-001: Idle/Ready — да; Running и смежные — нет.
    const bool measureOk = st == RunState::Idle || st == RunState::Ready;
    m_measure->setMeasureNowEnabled(measureOk);
}

void MainWindow::onMeasureNow()
{
    using afar::RunState;
    const auto st = static_cast<RunState>(m_state);
    if (st == RunState::Running || st == RunState::Pausing || st == RunState::Stopping
        || st == RunState::Finalizing) {
        m_connections->setDiagnostic(
            QStringLiteral("«Измерить сейчас» недоступно во время серии"));
        return;
    }
    if (!(m_measure->fStartHz() < m_measure->fStopHz())) {
        m_connections->setDiagnostic(QStringLiteral(
            "Диапазон не применён: f нач. должна быть меньше f кон.; прежний график сохранён. "
            "Для 1,160 ГГц введите 1160 МГц или 1,160 ГГц."));
        return;
    }
    setOperationProgress(QStringLiteral("Подготовка измерения S11/S21/S12/S22"), 0, 4);
    QMetaObject::invokeMethod(m_worker, "measureNow", Qt::QueuedConnection,
                              Q_ARG(double, m_measure->fStartHz()),
                              Q_ARG(double, m_measure->fStopHz()), Q_ARG(int, m_measure->points()),
                              Q_ARG(int, m_measure->ifbwHz()), Q_ARG(double, m_measure->powerDbm()),
                              Q_ARG(int, m_measure->averages()));
}

void MainWindow::onExportTwoPort()
{
    const QString selected = QFileDialog::getSaveFileName(
        this, QStringLiteral("Сохранить измерение и отчёт"),
        QStringLiteral("two-port-measurement.pdf"),
        QStringLiteral("Отчёт PDF (*.pdf);;Touchstone (*.s2p)"));
    if (selected.isEmpty()) {
        return;
    }
    m_measure->setTwoPortExportInProgress(true);
    setOperationProgress(QStringLiteral("Подготовка S2P и PDF-отчёта"), 0, 2);
    statusBar()->showMessage(
        QStringLiteral("Формирование S2P и PDF-отчёта… Не закрывайте программу."));
    const QVector<double> markers = m_measure->graphMarkerFrequenciesGhz();
    const QString deviceName = m_measure->reportDeviceName();
    const QString deviceSerial = m_measure->reportDeviceSerial();
    const QString operatorName = m_measure->reportOperatorName();
    const QString comment = m_measure->reportComment();
    const bool accepted = m_measure->reportAccepted();
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, selected, markers, deviceName, deviceSerial, operatorName,
         comment, accepted] {
            worker->exportTwoPort(selected, markers, deviceName, deviceSerial,
                                  operatorName, comment, accepted);
        },
        Qt::QueuedConnection);
}

void MainWindow::onCalibrateStep(int kind, int step, int port)
{
    onApplyVnaSettings();
    if (kind == 0) {
        QMetaObject::invokeMethod(m_worker, "calibrateOnePort", Qt::QueuedConnection,
                                  Q_ARG(int, step), Q_ARG(int, port));
    } else {
        QMetaObject::invokeMethod(m_worker, "calibrateTwoPort", Qt::QueuedConnection,
                                  Q_ARG(int, step));
    }
}

void MainWindow::onConnectionChanged(const QString& vnaModel,
                                     const QString& vnaAddress,
                                     bool vnaConnected,
                                     const QString& controllerIface,
                                     bool dutConnected,
                                     double temperatureC,
                                     bool temperatureValid,
                                     const QString& vnaSerial,
                                     const QString& vnaFirmware)
{
    m_connections->setVnaInfo(vnaModel, vnaAddress, vnaConnected, vnaSerial, vnaFirmware);
    m_connections->setControllerInfo(controllerIface, dutConnected);
    m_connections->setTemperatureC(temperatureC, temperatureValid);
}

void MainWindow::onProgress(qint64 completed, qint64 total, int channel, int attCode, int phaseCode)
{
    m_measure->setProgress(completed, total, channel, attCode, phaseCode);
    if (total > 0) {
        setOperationProgress(
            completed >= total
                ? QStringLiteral("Серия измерений завершена")
                : QStringLiteral("Серия: канал %1, ATT %2, PH %3")
                      .arg(channel).arg(attCode).arg(phaseCode),
            completed, total);
    }
}

void MainWindow::setOperationProgress(const QString& text, qint64 completed, qint64 total)
{
    m_operationText->setText(text);
    const int percent = total > 0
        ? static_cast<int>(qBound(qint64{0}, completed * 100 / total, qint64{100}))
        : 0;
    m_operationProgress->setValue(percent);
    m_operationProgress->setFormat(QStringLiteral("%1%").arg(percent));
}

void MainWindow::onEtaChanged(const QString& text)
{
    m_measure->setEtaText(text);
}

void MainWindow::onSweepPreview(const QVector<double>& freqGhz,
                                const QVector<double>& magDb,
                                const QVector<double>& phaseUnwrapDeg)
{
    m_measure->setSweepCurves(freqGhz, magDb, phaseUnwrapDeg);
}

void MainWindow::onSparamsPreview(const QVector<double>& freqGhz,
                                  const QVector<double>& s11mag,
                                  const QVector<double>& s11ph,
                                  const QVector<double>& s21mag,
                                  const QVector<double>& s21ph,
                                  const QVector<double>& s12mag,
                                  const QVector<double>& s12ph,
                                  const QVector<double>& s22mag,
                                  const QVector<double>& s22ph)
{
    m_measure->setSparamsCurves(freqGhz, s11mag, s11ph, s21mag, s21ph, s12mag, s12ph, s22mag,
                                s22ph);
}

void MainWindow::onSeriesArtifactsPreview(const QString& runId,
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
    m_measure->applySeriesArtifactsPreview(runId, completedStates, directPath, directValid,
                                           directTotal, directFlat, directFragment, inversePath,
                                           inverseValid, inverseTotal, inverseFlat, inverseFragment,
                                           reportPath, reportValid, reportFragment, manifestPath,
                                           manifestLines, manifestFragment);
}

void MainWindow::onDirectLutCurvePreview(const QVector<double>& freqGhz,
                                         const QVector<double>& magDb,
                                         const QVector<double>& phaseErrorDeg,
                                         int channel,
                                         int attCode,
                                         int phaseCode)
{
    m_measure->applyDirectLutCurvePreview(freqGhz, magDb, phaseErrorDeg, channel, attCode,
                                          phaseCode);
}

void MainWindow::onPaths(const QString& seriesRoot,
                         const QString& runConfig,
                         const QString& attenuatorCsv,
                         const QString& rawS21,
                         const QString& directLut,
                         const QString& inverseLut,
                         const QString& report,
                         const QString& manifest,
                         const QString& runEvents)
{
    m_formats->setSeriesPaths(seriesRoot, runConfig, attenuatorCsv, rawS21, directLut, inverseLut,
                              report, manifest, runEvents);
    m_measure->setSeriesArtifacts(seriesRoot, directLut, inverseLut, report, manifest);
}

void MainWindow::onMatrixSnapshot(int channel,
                                  int attCode,
                                  int measuringPhase,
                                  const QVector<int>& statuses,
                                  const QVector<int>& attempts,
                                  const QVector<int>& overloadFlags)
{
    m_matrix->applySnapshot(channel, attCode, measuringPhase, statuses, attempts, overloadFlags);
}

void MainWindow::onDiagnostic(const QString& text)
{
    m_connections->setDiagnostic(text);
}

void MainWindow::onMatrixSelectionChanged(int channel, int attCode)
{
    QMetaObject::invokeMethod(m_worker, "requestMatrixSnapshot", Qt::QueuedConnection,
                              Q_ARG(int, channel), Q_ARG(int, attCode));
}

void MainWindow::onRemeasureRequested(int channel, int attCode, const QVector<int>& phases)
{
    QMetaObject::invokeMethod(m_worker, "requestRemeasure", Qt::QueuedConnection,
                              Q_ARG(int, channel), Q_ARG(int, attCode),
                              Q_ARG(QVector<int>, phases));
}

void MainWindow::onCellInspectRequested(int channel, int attCode, int phase)
{
    QMetaObject::invokeMethod(m_worker, "requestCellPreview", Qt::QueuedConnection,
                              Q_ARG(int, channel), Q_ARG(int, attCode), Q_ARG(int, phase));
}

void MainWindow::onCellSweepPreview(const QVector<double>& freqGhz,
                                   const QVector<double>& magDb,
                                   const QVector<double>& phaseUnwrapDeg)
{
    m_matrix->setCellSweepCurves(freqGhz, magDb, phaseUnwrapDeg);
}

void MainWindow::onToggleTheme()
{
    const AppTheme next =
        currentAppTheme() == AppTheme::Dark ? AppTheme::Light : AppTheme::Dark;
    saveAppTheme(next);
    applyAppTheme(*qApp, next);
    refreshThemeButton();
    m_matrix->refreshTheme();
}

void MainWindow::refreshThemeButton()
{
    m_connections->setThemeButtonText(currentAppTheme() == AppTheme::Dark
                                          ? QStringLiteral("Тема: тёмная")
                                          : QStringLiteral("Тема: светлая"));
}

QString MainWindow::defaultDataRoot() const
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("data"));
}

void MainWindow::scanUnfinishedSeries()
{
    m_unfinishedSeries.clear();
    QDir root(defaultDataRoot());
    if (!root.exists()) {
        m_measure->setUnfinishedSeriesHint(QString());
        return;
    }
    const auto dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& name : dirs) {
        if (name.startsWith(QLatin1Char('_'))) {
            continue;
        }
        QDir series(root.filePath(name));
        if (series.exists(QStringLiteral("raw-s21.h5"))
            && !series.exists(QStringLiteral("report.pdf"))) {
            m_unfinishedSeries = series.absolutePath();
            break;
        }
    }
    m_measure->setUnfinishedSeriesHint(m_unfinishedSeries);
}

void MainWindow::onApplyVnaSettings()
{
    QMetaObject::invokeMethod(
        m_worker, "configureVna", Qt::QueuedConnection,
        Q_ARG(int, m_connections->vnaBackend()), Q_ARG(QString, m_connections->vnaHost()),
        Q_ARG(int, m_connections->vnaPort()), Q_ARG(QString, m_connections->vnaComPort()),
        Q_ARG(bool, m_connections->allowDirectAccess()),
        Q_ARG(int, m_connections->vnaConnectTimeoutMs()),
        Q_ARG(int, m_connections->vnaSweepTimeoutMs()),
        Q_ARG(int, m_connections->vnaMeasureRetries()));
    refreshDataSourceBadge();
}

void MainWindow::onApplyControllerSettings()
{
    QMetaObject::invokeMethod(
        m_worker, "configureController", Qt::QueuedConnection,
        Q_ARG(int, m_connections->controllerBackend()),
        Q_ARG(QString, m_connections->dutHost()), Q_ARG(int, m_connections->dutPort()),
        Q_ARG(QString, m_connections->dutComPort()));
}

void MainWindow::onProbeVna()
{
    beginVnaProbe(false);
}

void MainWindow::onConnectAndMeasureVna()
{
    beginVnaProbe(true);
}

void MainWindow::beginVnaProbe(bool measureAfterSuccess)
{
    m_measureAfterProbe = measureAfterSuccess;
    onApplyVnaSettings();
    onApplyControllerSettings();
    m_connections->setDiagnostic(
        QStringLiteral("Подключение к %1:%2…")
            .arg(m_connections->vnaHost())
            .arg(m_connections->vnaPort()));
    QMetaObject::invokeMethod(m_worker, "probeVna", Qt::QueuedConnection);
}

void MainWindow::refreshDataSourceBadge(bool probeOk, const QString& idnOrError)
{
    const int backend = m_connections->vnaBackend();
    const QString idn = idnOrError.trimmed();
    const bool idnLooksSim =
        idn.contains(QStringLiteral("SIM"), Qt::CaseInsensitive)
        || idn.contains(QStringLiteral("VnaSimulator"), Qt::CaseInsensitive);

    if (backend == 3) {
        m_connections->setDataSourceText(
            probeOk
                ? QStringLiteral("Источник: S2VNA DEMO C2220 (не метрология)")
                : QStringLiteral("Источник: S2VNA DEMO (связь не проверена)"));
        return;
    }
    if (backend == 0 || (probeOk && idnLooksSim)) {
        m_connections->setDataSourceText(
            QStringLiteral("Источник: имитатор (не метрология стенда)"));
        return;
    }

    if (probeOk && (backend == 1 || backend == 2) && !idn.isEmpty()) {
        QString brief = idn;
        if (brief.size() > 64) {
            brief = brief.left(61) + QStringLiteral("...");
        }
        m_connections->setDataSourceText(
            QStringLiteral("Источник: живой VNA · %1").arg(brief));
        return;
    }

    m_connections->setDataSourceText(
        QStringLiteral("Источник: VNA (связь не проверена)"));
}

void MainWindow::onProbeFinished(bool ok, const QString& idnOrError)
{
    const bool measureAfterSuccess = m_measureAfterProbe;
    m_measureAfterProbe = false;
    refreshDataSourceBadge(ok, idnOrError);
    if (ok) {
        m_connections->setDiagnostic(QStringLiteral("Связь OK: %1").arg(idnOrError));
        statusBar()->showMessage(QStringLiteral("VNA IDN: %1").arg(idnOrError), 8000);
        if (measureAfterSuccess) {
            onMeasureNow();
        }
    } else {
        m_connections->setDiagnostic(QStringLiteral("Нет связи: %1").arg(idnOrError));
        const bool demoMode = m_connections->vnaBackend() == 3;
        QMessageBox::warning(
            this, QStringLiteral("Проверка VNA"),
            demoMode
                ? QStringLiteral(
                      "Не удалось подключить S2VNA Demo C2220.\n\n%1\n\n"
                      "Запустите S2VNA вручную в Demo Mode, включите Socket Server "
                      "и повторите проверку. Физический анализатор не требуется.")
                      .arg(idnOrError)
                : QStringLiteral(
                      "Не удалось подключиться к C2220 через SCPI-сервер S2VNA.\n\n%1\n\n"
                      "Проверьте: S2VNA запущена, Socket Server включён (порт), "
                      "прибор подключен. Пока нет прибора — выберите «S2VNA Demo C2220».")
                      .arg(idnOrError));
    }
}

void MainWindow::onResumeSeries()
{
    scanUnfinishedSeries();
    if (m_unfinishedSeries.isEmpty()) {
        QMessageBox::information(
            this, QStringLiteral("Восстановление"),
            QStringLiteral("Незакрытая серия не найдена в каталоге данных."));
        return;
    }
    m_start->setEnabled(false);
    m_pause->setEnabled(false);
    m_stop->setEnabled(false);
    QMetaObject::invokeMethod(m_worker, "prepareRecovery", Qt::QueuedConnection,
                              Q_ARG(QString, m_unfinishedSeries));
}
