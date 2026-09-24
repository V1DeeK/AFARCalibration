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
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
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
    m_connections->setVnaInfo(QStringLiteral("PLANAR C1220/C2220"), QStringLiteral("VnaSimulator"),
                              false);
    m_connections->setControllerInfo(QStringLiteral("DutSimulator"), false);
    m_connections->setRunStatus(QStringLiteral("не настроена"), QStringLiteral("#666666"));

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
    m_wizardBtn = new QPushButton(QStringLiteral("Настроить серию АФАР"), cycle);
    m_wizardBtn->setObjectName(QStringLiteral("btnWizard"));
    auto* hint = new QLabel(
        QStringLiteral("Полная серия АФАР: требует мастер и контроллер изделия. "
                       "Для одиночного измерения S-параметра эта панель не нужна."),
        cycle);
    hint->setWordWrap(true);
    hint->setObjectName(QStringLiteral("hintLabel"));
    m_start = new QPushButton(QStringLiteral("Старт серии АФАР"), cycle);
    m_start->setObjectName(QStringLiteral("btnStart"));
    m_pause = new QPushButton(QStringLiteral("Пауза"), cycle);
    m_pause->setObjectName(QStringLiteral("btnPause"));
    m_stop = new QPushButton(QStringLiteral("Стоп"), cycle);
    m_stop->setObjectName(QStringLiteral("btnStop"));
    m_start->setEnabled(true);
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
    connect(m_worker, &MeasureWorker::singleSweepFinished, this,
            &MainWindow::onSingleSweepFinished);
    connect(m_worker, &MeasureWorker::csvSaveFinished, this, &MainWindow::onCsvSaveFinished);
    connect(m_worker, &MeasureWorker::calibrationFinished, this,
            &MainWindow::onCalibrationFinished);
    connect(m_worker, &MeasureWorker::filterMetricsChanged, m_measure,
            &MeasureTab::setFilterMetrics);
    connect(m_measure, &MeasureTab::openWizardRequested, this, &MainWindow::onOpenWizard);
    connect(m_measure, &MeasureTab::resumeSeriesRequested, this, &MainWindow::onResumeSeries);
    connect(m_measure, &MeasureTab::singleSweepRequested, this, &MainWindow::onSingleSweep);
    connect(m_measure, &MeasureTab::saveCsvRequested, this, &MainWindow::onSaveCsv);
    connect(m_measure, &MeasureTab::calibrationConnectionRequested, this,
            &MainWindow::onProbeVna);
    connect(m_measure, &MeasureTab::calibrationStepRequested, this,
            &MainWindow::onCalibrationStep);

    statusBar()->showMessage(
        QStringLiteral("CAL VNA: не проверена · THRU: — · Сырые данные: append-only · SHA-256"));

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
    const auto cfgPath = examples / "run-config.example.json";
    afar::RunConfig cfg;
    std::string diag;
    if (!examples.empty() && afar::RunConfig::loadFromFile(cfgPath, cfg, diag)) {
        m_measure->applyRunConfigDefaults(
            static_cast<double>(cfg.vna.f_start_hz) / 1e9,
            static_cast<double>(cfg.vna.f_stop_hz) / 1e9, cfg.vna.points, cfg.vna.ifbw_hz,
            cfg.vna.power_dbm, cfg.vna.averages);
        m_connections->setVnaInfo(QString::fromStdString(cfg.vna.model),
                                  QStringLiteral("VnaSimulator (не Socket)"), false);
        m_connections->setControllerInfo(QStringLiteral("DutSimulator"), false);
    } else {
        m_measure->applyRunConfigDefaults(4.9, 6.0, 201, 1000, -30.0, 8);
    }
}

void MainWindow::onOpenWizard()
{
    StartWizard wizard(this);
    wizard.setSweepPreset(m_measure->fStartGhz(), m_measure->fStopGhz(), m_measure->points(),
                          m_measure->ifbwHz(), m_measure->powerDbm(), m_measure->averages());
    wizard.setVnaEndpoint(m_connections->vnaHost(), m_connections->vnaPort());
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
    statusBar()->showMessage(
        QStringLiteral("CAL VNA: %1 · THRU: %2 · Сырые данные: append-only · SHA-256")
            .arg(wizard.calConfirmed() ? QStringLiteral("действительна (флаг)")
                                       : QStringLiteral("не проверена"),
                 wizard.thruSummary()));
    QMetaObject::invokeMethod(m_worker, "prepare", Qt::QueuedConnection,
                              Q_ARG(QString, wizard.dataRoot()),
                              Q_ARG(QString, wizard.runConfigPath()),
                              Q_ARG(QString, wizard.attenuatorCsvPath()),
                              Q_ARG(bool, wizard.forceSafeState()));
}

void MainWindow::onStart()
{
    using afar::RunState;
    const auto state = static_cast<RunState>(m_state);
    if (state == RunState::Paused) {
        QMetaObject::invokeMethod(m_worker, "resume", Qt::QueuedConnection);
        return;
    }
    if (state != RunState::Ready) {
        QMessageBox::information(
            this, QStringLiteral("Старт серии АФАР"),
            QStringLiteral(
                "Эта кнопка запускает полный перебор кодов АФАР, а не одиночное измерение "
                "фильтра. Сначала нажмите «Настроить серию АФАР» и завершите мастер.\n\n"
                "Для S11/S21/S12/S22 откройте этап 4 и нажмите "
                "«Измерить выбранный S-параметр». Для этого нужен только зелёный VNA."));
        return;
    }
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
    const bool running = st == RunState::Running || st == RunState::Pausing;
    const bool paused = st == RunState::Paused;
    m_start->setEnabled(!running);
    m_start->setText(paused ? QStringLiteral("Продолжить серию")
                            : QStringLiteral("Старт серии АФАР"));
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
}

void MainWindow::onProbeVna()
{
    onApplyVnaSettings();
    QMetaObject::invokeMethod(m_worker, "probeVna", Qt::QueuedConnection);
}

void MainWindow::onProbeFinished(bool ok, const QString& idnOrError)
{
    if (ok) {
        m_connections->setDiagnostic(QStringLiteral("Связь OK: %1").arg(idnOrError));
        m_measure->setCalibrationStatus(
            QStringLiteral("Соединение с VNA подтверждено: %1").arg(idnOrError));
        statusBar()->showMessage(QStringLiteral("VNA IDN: %1").arg(idnOrError), 8000);
    } else {
        m_connections->setDiagnostic(QStringLiteral("Нет связи: %1").arg(idnOrError));
        m_measure->setCalibrationStatus(
            QStringLiteral("Соединение с VNA не установлено: %1").arg(idnOrError));
        QMessageBox::warning(
            this, QStringLiteral("Проверка VNA"),
            QStringLiteral(
                "Не удалось подключиться к S2VNA/C1220/C2220.\n\n%1\n\n"
                "Проверьте: S2VNA запущена, Socket Server включён (порт), "
                "прибор подключен. Пока нет прибора — режим «Имитатор».")
                .arg(idnOrError));
    }
}

void MainWindow::onSingleSweep()
{
    onApplyVnaSettings();
    m_measure->setCsvAvailable(false);
    m_measure->setFilterMetrics(QStringLiteral("Метрики фильтра: измерение…"));
    const QString parameter = m_measure->sParameter();
    m_lastMeasuredParameter = parameter;
    const int parameterIndex = parameter == QStringLiteral("S11") ? 1
        : parameter == QStringLiteral("S12")                 ? 2
        : parameter == QStringLiteral("S22")                 ? 3
                                                                  : 0;
    QMetaObject::invokeMethod(
        m_worker, "measureSingleSweep", Qt::QueuedConnection,
        Q_ARG(double, m_measure->fStartGhz()), Q_ARG(double, m_measure->fStopGhz()),
        Q_ARG(int, m_measure->points()), Q_ARG(int, m_measure->ifbwHz()),
        Q_ARG(double, m_measure->powerDbm()), Q_ARG(int, m_measure->averages()),
        Q_ARG(int, parameterIndex));
}

void MainWindow::onSingleSweepFinished(bool ok, const QString& message)
{
    if (ok) {
        m_measure->setCsvAvailable(true);
        statusBar()->showMessage(QStringLiteral("Измерение готово: %1").arg(message), 10000);
    } else {
        m_measure->setFilterMetrics(QStringLiteral("Метрики фильтра: измерение не выполнено"));
        QMessageBox::warning(this, QStringLiteral("Измерение S-параметра"), message);
    }
}

void MainWindow::onSaveCsv()
{
    const QString parameter = m_lastMeasuredParameter.toLower();
    const QString name = QStringLiteral("filter-%1-%2.csv")
                             .arg(parameter,
                                  QDateTime::currentDateTime().toString(
                                      QStringLiteral("yyyyMMdd-HHmmss")));
    const QString initial = QDir(QStandardPaths::writableLocation(
                                     QStandardPaths::DocumentsLocation))
                                .filePath(name);
    const QString csvPath = QFileDialog::getSaveFileName(
        this, QStringLiteral("Сохранить последнее измерение"), initial,
        QStringLiteral("CSV (*.csv)"));
    if (!csvPath.isEmpty()) {
        QMetaObject::invokeMethod(m_worker, "saveLastSweepCsv", Qt::QueuedConnection,
                                  Q_ARG(QString, csvPath));
    }
}

void MainWindow::onCsvSaveFinished(bool ok, const QString& message, const QString& csvPath)
{
    if (ok) {
        statusBar()->showMessage(QStringLiteral("CSV сохранён: %1").arg(csvPath), 10000);
        QMessageBox::information(this, QStringLiteral("Сохранение CSV"),
                                 QStringLiteral("%1\n\n%2").arg(message, csvPath));
    } else {
        QMessageBox::warning(this, QStringLiteral("Сохранение CSV"), message);
    }
}

void MainWindow::onCalibrationStep(int step)
{
    static const QStringList prompts = {
        QStringLiteral("Будут применены параметры этапа 4 и начата полная SOLT-калибровка "
                       "портов 1 и 2. В S2VNA должен быть выбран правильный комплект мер."),
        QStringLiteral("Подключите меру «Открытый канал» к порту 1."),
        QStringLiteral("Подключите меру КЗ к порту 1."),
        QStringLiteral("Подключите согласованную нагрузку 50 Ом к порту 1."),
        QStringLiteral("Подключите меру «Открытый канал» к порту 2."),
        QStringLiteral("Подключите меру КЗ к порту 2."),
        QStringLiteral("Подключите согласованную нагрузку 50 Ом к порту 2."),
        QStringLiteral("Соедините порты 1 и 2 калибровочной перемычкой."),
        QStringLiteral("Применить собранные коэффициенты двухпортовой калибровки?"),
    };
    if (step < 0 || step >= prompts.size()) {
        return;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Двухпортовая калибровка"),
        prompts[step] + QStringLiteral("\n\nПродолжить измерение этого шага?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    if (step == 0) {
        onApplyVnaSettings();
    }
    m_measure->setCalibrationStatus(QStringLiteral("Калибровка: выполняется шаг %1…").arg(step + 1));
    QMetaObject::invokeMethod(
        m_worker, "runCalibrationStep", Qt::QueuedConnection, Q_ARG(int, step),
        Q_ARG(double, m_measure->fStartGhz()), Q_ARG(double, m_measure->fStopGhz()),
        Q_ARG(int, m_measure->points()), Q_ARG(int, m_measure->ifbwHz()),
        Q_ARG(double, m_measure->powerDbm()), Q_ARG(int, m_measure->averages()));
}

void MainWindow::onCalibrationFinished(bool ok, int step, const QString& message)
{
    m_measure->setCalibrationStatus(
        QStringLiteral("Калибровка, шаг %1: %2").arg(step + 1).arg(message));
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Двухпортовая калибровка"), message);
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
