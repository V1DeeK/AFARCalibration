#pragma once

#include "S2VnaRuntime.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QString>
#include <QVector>

class QCloseEvent;
class AcceptanceAtTab;
class BoardParametersPage;
class CodeMatrixTab;
class ConnectionBar;
class MeasureTab;
class MeasureWorker;
class PsServiceCheckPage;
class PsSession;
class SettingsTab;
class SpectrumOpenGLWidget;
class TestRunTab;
class QTabWidget;
class QLabel;
class QProgressBar;
class QPushButton;
class QThread;
class QTimer;
class SpectrumEngine;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(SpectrumEngine& spectrumEngine, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onPrepareFinished(bool ok, const QString& diagnostics);
    void onStateChanged(int state, const QString& russianText, const QString& colorName);
    void onConnectionChanged(const QString& vnaModel,
                             const QString& vnaAddress,
                             bool vnaConnected,
                             const QString& controllerIface,
                             bool dutConnected,
                             double temperatureC,
                             bool temperatureValid,
                             const QString& vnaSerial,
                             const QString& vnaFirmware);
    void onProgress(qint64 completed, qint64 total, int channel, int attCode, int phaseCode);
    void onPaths(const QString& seriesRoot,
                 const QString& runConfig,
                 const QString& attenuatorCsv,
                 const QString& rawS21,
                 const QString& directLut,
                 const QString& inverseLut,
                 const QString& report,
                 const QString& manifest,
                 const QString& runEvents);
    void onMatrixSnapshot(int channel,
                          int attCode,
                          int measuringPhase,
                          const QVector<int>& statuses,
                          const QVector<int>& attempts,
                          const QVector<int>& overloadFlags);
    void onDiagnostic(const QString& text);
    void onMatrixSelectionChanged(int channel, int attCode);
    void onRemeasureRequested(int channel, int attCode, const QVector<int>& phases);
    void onCellInspectRequested(int channel, int attCode, int phase);
    void onCellSweepPreview(const QVector<double>& freqGhz,
                            const QVector<double>& magDb,
                            const QVector<double>& phaseUnwrapDeg);
    void onToggleTheme();
    void onApplyVnaSettings();
    void onApplyControllerSettings();
    void onToggleVnaConnection(bool enable);
    void onProbeFinished(bool ok, const QString& idnOrError);
    void onVnaDisconnected();
    void refreshDataSourceBadge(bool probeOk = false, const QString& idnOrError = {});
    void onEtaChanged(const QString& text);
    void onSweepPreview(const QVector<double>& freqGhz,
                        const QVector<double>& magDb,
                        const QVector<double>& phaseUnwrapDeg);
    void onSparamsPreview(const QVector<double>& freqGhz,
                          const QVector<double>& s11mag,
                          const QVector<double>& s11ph,
                          const QVector<double>& s21mag,
                          const QVector<double>& s21ph,
                          const QVector<double>& s12mag,
                          const QVector<double>& s12ph,
                          const QVector<double>& s22mag,
                          const QVector<double>& s22ph);
    void onSeriesArtifactsPreview(const QString& runId,
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
                                  const QString& manifestFragment);
    void onDirectLutCurvePreview(const QVector<double>& freqGhz,
                                 const QVector<double>& magDb,
                                 const QVector<double>& phaseErrorDeg,
                                 int channel,
                                 int attCode,
                                 int phaseCode);
    void updateCycleButtons(int state);
    void onMeasureNow();
    void onExportTwoPort();
    void onCalibrateStep(int kind, int step, int port);

private:
    void loadExampleDefaults();
    void refreshThemeButton();
    void beginVnaProbe(bool measureAfterSuccess);
    void setOperationProgress(const QString& text, qint64 completed, qint64 total);
    void updateModuleStatus();
    void updateSpectrumFromPs();
    [[nodiscard]] QString defaultDataRoot() const;

    SpectrumEngine* m_spectrumEngine = nullptr;

    ConnectionBar* m_connections = nullptr;
    MeasureTab* m_measure = nullptr;
    CodeMatrixTab* m_matrix = nullptr;
    AcceptanceAtTab* m_acceptance = nullptr;
    PsSession* m_psSession = nullptr;
    QTabWidget* m_tabs = nullptr;
    QPushButton* m_themeButton = nullptr;
    SettingsTab* m_settingsTab = nullptr;
    BoardParametersPage* m_boardPage = nullptr;
    SpectrumOpenGLWidget* m_spectrum = nullptr;
    QWidget* m_spectrumPage = nullptr;  ///< страница «Спектр» (без OpenGL — пояснение)
    QTimer* m_spectrumConnectTimer = nullptr;
    bool m_spectrumServiceRequested = false;
    bool m_spectrumConnectedFromPs = false;
    TestRunTab* m_testRun = nullptr;
    PsServiceCheckPage* m_psCheck = nullptr;
    QLabel* m_operationText = nullptr;
    QProgressBar* m_operationProgress = nullptr;
    QLabel* m_moduleStatus = nullptr;

    QThread* m_thread = nullptr;
    MeasureWorker* m_worker = nullptr;
    int m_state = 0;
    bool m_vnaConnected = false;
    bool m_connectionRequested = false;
    bool m_disconnectPending = false;
    bool m_measureAfterProbe = false;
    bool m_autoProbePending = false;
    bool m_reusedExternalS2Vna = false;
    QElapsedTimer m_autoProbeTimer;
    S2VnaRuntime m_s2vnaRuntime;
};
