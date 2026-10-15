#include "MainWindow.h"

#include "AcceptanceAtTab.h"
#include "CodeMatrixTab.h"
#include "ConnectionBar.h"
#include "MeasureTab.h"
#include "MeasureWorker.h"
#include "BoardParametersPage.h"
#include "ConnectionTab.h"
#include "ParametersTab.h"
#include "PsServiceCheckPage.h"
#include "PsSession.h"
#include "TestRunTab.h"
#include "UiStyle.h"
#include "RunConfig.h"
#include "RunStateMachine.h"
#include "afar/C2220Limits.h"
#include "Theme.h"
#include "spectrum_engine.hpp"

#include <mutex>
#include "spectrum_opengl_widget.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDir>
#include <QDebug>
#include <QFrame>
#include <QFileDialog>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <filesystem>
#include <string>

#ifndef AFAR_EXAMPLES_DIR
#define AFAR_EXAMPLES_DIR ""
#endif

namespace {

/// Каталог примеров: рядом с exe (Windows/Linux-поставка), в Resources
/// бандла macOS, в share/ после установки; иначе — исходники сборки.
std::filesystem::path examplesDirectory()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const auto* relative : {"examples", "../Resources/examples",
                                 "../share/AfarRxCalibrationStudio/examples"}) {
        const QString candidate = QDir::cleanPath(appDir.filePath(QString::fromLatin1(relative)));
        if (QFileInfo(candidate).isDir()) {
#ifdef _WIN32
            return std::filesystem::path(candidate.toStdWString());
#else
            return std::filesystem::path(candidate.toStdString());
#endif
        }
    }
    return std::filesystem::path(AFAR_EXAMPLES_DIR);
}

}  // namespace

MainWindow::MainWindow(SpectrumEngine& spectrumEngine, QWidget* parent)
    : QMainWindow(parent), m_spectrumEngine(&spectrumEngine)
{
    setWindowTitle(QStringLiteral("AFAR RX Calibration Studio / Spectrum Analyzer"));

    // Адаптивность: окно не больше доступной области экрана (ноутбук 14–15″ с масштабом 125–150 %,
    // монитор 24″ и больше), страницы вкладок прокручиваются, а не растягивают окно.
    const QRect workArea = screen() ? screen()->availableGeometry()
                                    : QGuiApplication::primaryScreen()->availableGeometry();
    setMinimumSize(qMin(900, workArea.width()), qMin(560, workArea.height()));
    const int width = qBound(minimumWidth(), static_cast<int>(workArea.width() * 0.88), 1700);
    const int height = qBound(minimumHeight(), static_cast<int>(workArea.height() * 0.88), 1050);
    resize(width, height);
    move(workArea.center() - rect().center());
    // Прошлый размер и место окна, если они ещё помещаются на текущем экране.
    const QByteArray savedGeometry = QSettings().value(QStringLiteral("ui/window_geometry_v1")).toByteArray();
    if (!savedGeometry.isEmpty()) {
        restoreGeometry(savedGeometry);
        if (!workArea.intersects(frameGeometry()) || this->width() > workArea.width() || this->height() > workArea.height()) {
            resize(width, height);
            move(workArea.center() - rect().center());
        }
    }

    m_psSession = new PsSession(this);
    // Параметры платы запрашиваем сразу после подключения PS; спектр подключится к службе libiio сам,
    // когда она ответит (см. BoardParametersPage).
    connect(m_psSession->link(), &PsLink::stateChanged, this, [this] {
        if (!m_psSession->link()->isReady()) {
            return;
        }
        QTimer::singleShot(500, this, [this] {
            if (m_boardPage != nullptr && !m_boardPage->busy() && m_psSession->link()->isReady()) {
                m_boardPage->requestRead();
            }
        });
    });

    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("mainTabs"));
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setObjectName(QStringLiteral("mainTabBar"));
    m_tabs->tabBar()->setDrawBase(false);
    m_tabs->tabBar()->setExpanding(false);

    m_themeButton = new QPushButton(m_tabs);
    m_themeButton->setObjectName(QStringLiteral("btnThemeToggle"));
    m_themeButton->setToolTip(QStringLiteral("Светлая или тёмная тема всей программы, включая спектр"));
    m_tabs->setCornerWidget(m_themeButton, Qt::TopRightCorner);
    connect(m_themeButton, &QPushButton::clicked, this, &MainWindow::onToggleTheme);

    // --- Подключение ---
    m_connectionTab = new ConnectionTab(m_psSession, m_tabs);
    m_boardPage = new BoardParametersPage(m_psSession);
    m_connectionTab->setTechPage(m_boardPage);
    connect(m_boardPage, &BoardParametersPage::operationProgress, this,
            [this](const QString& text, int done, int total) { setOperationProgress(text, done, total); });

    // --- Установка параметров ---
    m_parametersTab = new ParametersTab(m_psSession, m_tabs);

    // --- Измерение S-параметров: всё, что было в «Калибровке AFAR» ---
    auto* viewport = new QScrollArea(m_tabs);
    viewport->setObjectName(QStringLiteral("sParametersWorkspace"));
    viewport->setWidgetResizable(true);
    viewport->setFrameShape(QFrame::NoFrame);

    auto* root = new QWidget(viewport);
    auto* rootLayout = new QVBoxLayout(root);
    rootLayout->setContentsMargins(ui::kPageMargin, ui::kPageMargin, ui::kPageMargin, ui::kPageMargin);
    rootLayout->setSpacing(ui::kGap);

    m_connections = new ConnectionBar(root);
    m_connections->setVnaInfo(QStringLiteral("PLANAR C2220"), QStringLiteral("VnaSimulator"),
                              false);
    m_connections->setControllerInfo(QStringLiteral("DutSimulator"), false);
    m_connections->setRunStatus(QStringLiteral("простой"), QStringLiteral("#333333"));

    m_measure = new MeasureTab(root);
    // Код-матрица работает только в серии 64×64 (контроллера нет): вкладки нет, объект нужен связям серии.
    m_matrix = new CodeMatrixTab(root);
    m_matrix->hide();

    // Третья страница общей области рядом с подключением и контроллером.
    auto* calBox = new QWidget(root);
    calBox->setObjectName(QStringLiteral("calibrationBox"));
    auto* calLayout = new QVBoxLayout(calBox);
    calLayout->setContentsMargins(0, 0, 0, 0);
    calLayout->addWidget(m_measure->calibrationPanel());
    m_connections->setMiddleWidget(calBox);
    // Очередь ошибок SCPI показывается в «Тех. состоянии» на вкладке «Подключение».
    m_connectionTab->addTechSection(m_connections->scpiPanel());

    rootLayout->addWidget(m_connections);
    rootLayout->addWidget(m_measure, 1);
    viewport->setWidget(root);

    // --- Спектр ---
    // Без OpenGL (платформа offscreen в автотестах) вкладка показывает пояснение вместо спектра.
    QWidget* spectrumPage = nullptr;
    if (QGuiApplication::platformName() == QLatin1String("offscreen")) {
        auto* note = new QLabel(QStringLiteral("OpenGL на этой платформе недоступен — спектр не показывается."), m_tabs);
        note->setAlignment(Qt::AlignCenter);
        spectrumPage = note;
    } else {
        m_spectrum = new SpectrumOpenGLWidget(spectrumEngine.state(), m_tabs);
        m_spectrum->setObjectName(QStringLiteral("spectrumWorkspace"));
        spectrumPage = m_spectrum;
    }

    // --- Тестовый прогон ---
    m_testRun = new TestRunTab(m_psSession, m_tabs);

    // --- Приёмка ---
    auto* acceptance = new QTabWidget(m_tabs);
    acceptance->setObjectName(QStringLiteral("acceptanceTabs"));
    m_psCheck = new PsServiceCheckPage(m_psSession->link(), acceptance);
    m_acceptance = new AcceptanceAtTab(acceptance);
    acceptance->addTab(m_psCheck, QStringLiteral("Приёмочная проверка PS"));
    acceptance->addTab(m_acceptance, QStringLiteral("Приёмка AT"));
    connect(m_psCheck, &PsServiceCheckPage::runningChanged, m_psSession, &PsSession::setCheckRunning);
    const auto refreshCheck = [this] {
        m_psCheck->setAllowed(m_psSession->link()->isReady() && !m_psSession->seriesActive());
    };
    connect(m_psSession, &PsSession::controlsChanged, this, refreshCheck);
    refreshCheck();

    // Страницы прокручиваются: при тесной рамке окна появляется прокрутка, а виджеты не налезают друг на друга.
    m_tabs->addTab(ui::scrollPage(m_connectionTab), QStringLiteral("Подключение"));
    m_tabs->addTab(ui::scrollPage(m_parametersTab), QStringLiteral("Установка параметров"));
    m_tabs->addTab(viewport, QStringLiteral("Измерение S-параметров"));
    m_tabs->addTab(spectrumPage, QStringLiteral("Спектр"));
    m_tabs->addTab(ui::scrollPage(m_testRun), QStringLiteral("Тестовый прогон"));
    m_tabs->addTab(ui::scrollPage(acceptance), QStringLiteral("Приёмка"));
    setCentralWidget(m_tabs);

    const QString tabKey = QStringLiteral("ui/active_tab_v3");
    m_tabs->setCurrentIndex(qBound(0, QSettings().value(tabKey, 0).toInt(), m_tabs->count() - 1));
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [tabKey](int index) { QSettings().setValue(tabKey, index); });
    m_spectrumConnectTimer = new QTimer(this);
    m_spectrumConnectTimer->setInterval(3000);
    connect(m_spectrumConnectTimer, &QTimer::timeout, this, &MainWindow::updateSpectrumFromPs);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::updateSpectrumFromPs);
    connect(m_psSession, &PsSession::modeChanged, this, &MainWindow::updateSpectrumFromPs);
    connect(m_psSession->link(), &PsLink::stateChanged, this, &MainWindow::updateSpectrumFromPs);

    m_operationText = new QLabel(QStringLiteral("Готово"), this);
    m_operationText->setObjectName(QStringLiteral("operationStatusText"));
    m_operationText->setMinimumWidth(0);
    m_operationText->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_operationProgress = new QProgressBar(this);
    m_operationProgress->setObjectName(QStringLiteral("operationProgressBar"));
    m_operationProgress->setRange(0, 100);
    m_operationProgress->setValue(0);
    m_operationProgress->setFixedWidth(180);
    m_operationProgress->setVisible(false);
    m_moduleStatus = new QLabel(this);
    m_moduleStatus->setObjectName(QStringLiteral("moduleStatusText"));
    m_moduleStatus->setMaximumWidth(520);
    statusBar()->addWidget(m_moduleStatus);
    statusBar()->addPermanentWidget(m_operationText, 1);
    statusBar()->addPermanentWidget(m_operationProgress);


    m_thread = new QThread(this);
    m_worker = new MeasureWorker();
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);


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
    connect(m_measure, &MeasureTab::measureNowRequested, this, &MainWindow::onMeasureNow);
    connect(m_measure, &MeasureTab::exportTwoPortRequested, this, &MainWindow::onExportTwoPort);
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
                     setOperationProgress(QStringLiteral("Ошибка измерения"), 1, 1);
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
                     setOperationProgress(QStringLiteral("Ошибка формирования отчёта"), 1, 1);
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

    m_thread->start();
    loadExampleDefaults();
    refreshThemeButton();
    if (m_spectrum) {
        m_spectrum->setLightTheme(currentAppTheme() == AppTheme::Light);
    }
    // Автоподключение: оператор подключался и не нажимал «Отключиться».
    m_psSession->connectFromSettings();
    updateSpectrumFromPs();
    onApplyVnaSettings();
    onApplyControllerSettings();

    auto* moduleStatusTimer = new QTimer(this);
    moduleStatusTimer->setInterval(500);
    connect(moduleStatusTimer, &QTimer::timeout, this, &MainWindow::updateModuleStatus);
    moduleStatusTimer->start();
    updateModuleStatus();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    QSettings().setValue(QStringLiteral("ui/window_geometry_v1"), saveGeometry());
    QMainWindow::closeEvent(event);
}

void MainWindow::updateSpectrumFromPs()
{
    const bool ready = m_psSession->link()->isReady();
    const bool spectrumTab = m_tabs->currentIndex() == 3;
    if (m_spectrumConnectedFromPs
        && (!ready || (m_psSession->modeKnown()
                       && m_psSession->mode() != afar::ps::Mode::Service))) {
        m_spectrumEngine->state().disconnect_requested.store(true);
        m_spectrumConnectedFromPs = false;
    }
    if (!ready || !spectrumTab || m_psSession->seriesActive() || m_psSession->checkRunning()) {
        m_spectrumConnectTimer->stop();
        if (!ready || !spectrumTab) {
            m_spectrumServiceRequested = false;
        }
        return;
    }
    if (!m_psSession->modeKnown()) {
        return;
    }
    if (m_psSession->mode() == afar::ps::Mode::Idle) {
        if (!m_spectrumServiceRequested && m_psSession->state() == QStringLiteral("READY")) {
            m_spectrumServiceRequested = true;
            if (m_psSession->send(afar::ps::msg::SetMode, {{"mode", "SERVICE"}}) >= 0) {
                statusBar()->showMessage(QStringLiteral("Спектр: перевод платы в сервисный режим…"));
            }
        }
        return;
    }
    if (m_psSession->mode() != afar::ps::Mode::Service) {
        return;
    }
    auto& spectrum = m_spectrumEngine->state();
    const std::string uri = "ip:" + m_psSession->link()->host().toStdString();
    {
        std::lock_guard<std::mutex> lock(spectrum.connection_mutex);
        if (spectrum.pluto_ip != uri) {
            spectrum.pluto_ip = uri;
            if (spectrum.is_connected.load()) {
                spectrum.connect_requested.store(true);
            }
        }
    }
    if (!spectrum.is_connected.load() && !spectrum.connect_requested.load()
        && !spectrum.connect_in_progress.load()) {
        spectrum.connect_requested.store(true);
        m_spectrumConnectedFromPs = true;
        statusBar()->showMessage(QStringLiteral("Спектр: подключение к %1…")
                                     .arg(m_psSession->link()->host()));
    }
    if (!m_spectrumConnectTimer->isActive()) {
        m_spectrumConnectTimer->start();
    }
    if (spectrum.is_connected.load()) {
        m_spectrumConnectTimer->stop();
    }
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
    const std::filesystem::path examples = examplesDirectory();
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
    updateModuleStatus();
}

void MainWindow::updateCycleButtons(int state)
{
    using afar::RunState;
    const auto st = static_cast<RunState>(state);
    // UI-MEAS-001: Idle/Ready — да; Running и смежные — нет.
    const bool measureOk = st == RunState::Idle || st == RunState::Ready;
    m_measure->setMeasureNowEnabled(measureOk);
    // Связь вкладки PS уступает серии, только если серия сама пишет коды
    // через PS (контроллер «PS изделия»): PS держит одно соединение. Серия с
    // любым другим контроллером работает только с анализатором и PS не трогает.
    const bool idle = st == RunState::Idle || st == RunState::Complete
        || st == RunState::Aborted || st == RunState::Error;
    const bool seriesUsesPs = m_connections->controllerBackend() == MeasureWorker::CtrlPs;
    m_psSession->setSeriesActive(!idle && seriesUsesPs);
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
    m_vnaConnected = vnaConnected;
    m_connections->setVnaInfo(vnaModel, vnaAddress, vnaConnected, vnaSerial, vnaFirmware);
    m_connections->setControllerInfo(controllerIface, dutConnected);
    m_connections->setTemperatureC(temperatureC, temperatureValid);
    updateModuleStatus();
}

void MainWindow::updateModuleStatus()
{
    if (!m_moduleStatus || !m_spectrumEngine) {
        return;
    }

    using afar::RunState;
    const auto runState = static_cast<RunState>(m_state);
    const bool measuring = runState == RunState::Running || runState == RunState::Pausing
        || runState == RunState::Paused || runState == RunState::Stopping
        || runState == RunState::Finalizing;

    const AppState& spectrum = m_spectrumEngine->state();
    QString spectrumState = QStringLiteral("SDR не подключён");
    if (spectrum.is_playing_back.load()) {
        spectrumState = QStringLiteral("IQ playback");
    } else if (spectrum.is_connected.load()) {
        spectrumState = QStringLiteral("SDR подключён");
    }
    if (spectrum.is_saving.load() || spectrum.tv_is_recording.load()) {
        spectrumState += QStringLiteral(", запись");
    }

    const QString vnaState = m_vnaConnected ? QStringLiteral("подключён")
                                            : QStringLiteral("не подключён");
    const QString shortSpectrum = spectrum.is_playing_back.load()
        ? QStringLiteral("воспроизведение")
        : spectrum.is_connected.load() ? QStringLiteral("подключён")
                                       : QStringLiteral("не подключён");
    const QString psState = m_psSession->link()->isReady() ? QStringLiteral("на связи")
                                                           : QStringLiteral("нет связи");
    m_moduleStatus->setText(
        QStringLiteral("PS: %1 · VNA: %2 · SDR: %3").arg(psState, vnaState, shortSpectrum));
    m_moduleStatus->setToolTip(
        QStringLiteral("AFAR: %1%2\nАнализатор спектра: %3")
            .arg(m_vnaConnected ? QStringLiteral("VNA подключён") : QStringLiteral("VNA не подключён"),
                 measuring ? QStringLiteral(", измерение") : QString(), spectrumState));
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
    m_operationText->setToolTip(text);
    const int percent = total > 0
        ? static_cast<int>(qBound(qint64{0}, completed * 100 / total, qint64{100}))
        : 0;
    m_operationProgress->setVisible(total <= 0 || completed < total);
    m_operationProgress->setRange(total > 0 ? 0 : 0, total > 0 ? 100 : 0);
    m_operationProgress->setValue(percent);
    m_operationProgress->setFormat(total > 0 ? QStringLiteral("%1%").arg(percent) : QString());
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
    (void)runConfig;
    (void)attenuatorCsv;
    (void)rawS21;
    (void)runEvents;
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
    if (m_spectrum) {
        m_spectrum->setLightTheme(next == AppTheme::Light);
    }
}

void MainWindow::refreshThemeButton()
{
    const bool dark = currentAppTheme() == AppTheme::Dark;
    m_connections->setThemeButtonText(dark ? QStringLiteral("Тема: тёмная")
                                           : QStringLiteral("Тема: светлая"));
    m_themeButton->setText(dark ? QStringLiteral("Светлая тема") : QStringLiteral("Тёмная тема"));
}

QString MainWindow::defaultDataRoot() const
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(base).filePath(QStringLiteral("data"));
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

