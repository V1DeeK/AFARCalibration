#include "MainWindow.h"

#include "CodeMatrixTab.h"
#include "ConnectionBar.h"
#include "DataFormatsTab.h"
#include "MeasureTab.h"
#include "MeasureWorker.h"
#include "RunConfig.h"
#include "RunStateMachine.h"
#include "StartWizard.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QVBoxLayout>

#include <filesystem>
#include <string>

#ifndef AFAR_EXAMPLES_DIR
#define AFAR_EXAMPLES_DIR ""
#endif

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("AFAR RX Calibration Studio"));
    resize(1280, 800);

    auto* root = new QWidget(this);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);

    m_connections = new ConnectionBar(root);
    m_connections->setVnaInfo(QStringLiteral("PLANAR C2220"), QStringLiteral("VnaSimulator"),
                              false);
    m_connections->setControllerInfo(QStringLiteral("DutSimulator"), false);
    m_connections->setRunStatus(QStringLiteral("простой"), QStringLiteral("#333333"));

    auto* tabs = new QTabWidget(root);
    m_measure = new MeasureTab(tabs);
    m_matrix = new CodeMatrixTab(tabs);
    m_formats = new DataFormatsTab(tabs);
    tabs->addTab(m_measure, QStringLiteral("Измерение"));
    tabs->addTab(m_matrix, QStringLiteral("Матрица кодов"));
    tabs->addTab(m_formats, QStringLiteral("Форматы данных"));

    auto* cycle = new QFrame(root);
    cycle->setFrameShape(QFrame::StyledPanel);
    auto* cycleLayout = new QHBoxLayout(cycle);
    m_wizardBtn = new QPushButton(QStringLiteral("Мастер запуска"), cycle);
    m_wizardBtn->setObjectName(QStringLiteral("btnWizard"));
    auto* hint = new QLabel(
        QStringLiteral("Имитаторы. Синяя «Мастер запуска» → галочки «Далее» → "
                       "«Готово». Потом зелёная «Старт». Этапы слева — разные экраны."),
        cycle);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("hintLabel"));
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
    cycleLayout->addWidget(hint, 1);
    cycleLayout->addWidget(m_start);
    cycleLayout->addWidget(m_pause);
    cycleLayout->addWidget(m_stop);

    rootLayout->addWidget(m_connections);
    rootLayout->addWidget(tabs, 1);
    rootLayout->addWidget(cycle);
    setCentralWidget(root);

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
    connect(m_matrix, &CodeMatrixTab::selectionChanged, this,
            &MainWindow::onMatrixSelectionChanged);
    connect(m_matrix, &CodeMatrixTab::remeasureRequested, this,
            &MainWindow::onRemeasureRequested);
    connect(m_matrix, &CodeMatrixTab::cellInspectRequested, this,
            &MainWindow::onCellInspectRequested);
    connect(m_worker, &MeasureWorker::cellSweepPreview, this, &MainWindow::onCellSweepPreview);
    connect(m_connections, &ConnectionBar::themeToggleRequested, this, &MainWindow::onToggleTheme);
    connect(m_connections, &ConnectionBar::vnaSettingsChanged, this, &MainWindow::onApplyVnaSettings);
    connect(m_connections, &ConnectionBar::probeVnaRequested, this, &MainWindow::onProbeVna);
    connect(m_worker, &MeasureWorker::probeFinished, this, &MainWindow::onProbeFinished);
    connect(m_worker, &MeasureWorker::seriesArtifactsPreview, this,
            &MainWindow::onSeriesArtifactsPreview);
    connect(m_worker, &MeasureWorker::directLutCurvePreview, this,
            &MainWindow::onDirectLutCurvePreview);
    connect(m_measure, &MeasureTab::openWizardRequested, this, &MainWindow::onOpenWizard);
    connect(m_measure, &MeasureTab::resumeSeriesRequested, this, &MainWindow::onResumeSeries);

    statusBar()->showMessage(
        QStringLiteral("ВАЦ (S2VNA): не подтверждена · THRU: — · Сырые данные: append-only · SHA-256"));

    m_thread->start();
    loadExampleDefaults();
    refreshThemeButton();
    scanUnfinishedSeries();
    onApplyVnaSettings();
}

MainWindow::~MainWindow()
{
    if (m_thread && m_worker) {
        QMetaObject::invokeMethod(m_worker, "shutdown", Qt::BlockingQueuedConnection);
        m_thread->quit();
        if (!m_thread->wait(5000)) {
            m_thread->terminate();
            m_thread->wait(1000);
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
}

void MainWindow::onConnectionChanged(const QString& vnaModel,
                                     const QString& vnaAddress,
                                     bool vnaConnected,
                                     const QString& controllerIface,
                                     bool dutConnected,
                                     double temperatureC,
                                     bool temperatureValid)
{
    m_connections->setVnaInfo(vnaModel, vnaAddress, vnaConnected);
    m_connections->setControllerInfo(controllerIface, dutConnected);
    m_connections->setTemperatureC(temperatureC, temperatureValid);
}

void MainWindow::onProgress(qint64 completed, qint64 total, int channel, int attCode, int phaseCode)
{
    m_measure->setProgress(completed, total, channel, attCode, phaseCode);
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
        Q_ARG(bool, m_connections->allowDirectAccess()));
    refreshDataSourceBadge();
}

void MainWindow::onProbeVna()
{
    onApplyVnaSettings();
    QMetaObject::invokeMethod(m_worker, "probeVna", Qt::QueuedConnection);
}

void MainWindow::refreshDataSourceBadge(bool probeOk, const QString& idnOrError)
{
    const int backend = m_connections->vnaBackend();
    const QString idn = idnOrError.trimmed();
    const bool idnLooksSim =
        idn.contains(QStringLiteral("SIM"), Qt::CaseInsensitive)
        || idn.contains(QStringLiteral("VnaSimulator"), Qt::CaseInsensitive);

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
    refreshDataSourceBadge(ok, idnOrError);
    if (ok) {
        m_connections->setDiagnostic(QStringLiteral("Связь OK: %1").arg(idnOrError));
        statusBar()->showMessage(QStringLiteral("VNA IDN: %1").arg(idnOrError), 8000);
    } else {
        m_connections->setDiagnostic(QStringLiteral("Нет связи: %1").arg(idnOrError));
        QMessageBox::warning(
            this, QStringLiteral("Проверка VNA"),
            QStringLiteral(
                "Не удалось подключиться к S2VNA/C2220.\n\n%1\n\n"
                "Проверьте: S2VNA запущена, Socket Server включён (порт), "
                "прибор подключен. Пока нет прибора — режим «Имитатор».")
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
