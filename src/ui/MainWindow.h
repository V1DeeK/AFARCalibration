#pragma once

#include <QMainWindow>
#include <QString>
#include <QVector>

class CodeMatrixTab;
class ConnectionBar;
class DataFormatsTab;
class MeasureTab;
class MeasureWorker;
class QPushButton;
class QThread;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void onOpenWizard();
    void onStart();
    void onPause();
    void onStop();
    void onPrepareFinished(bool ok, const QString& diagnostics);
    void onStateChanged(int state, const QString& russianText, const QString& colorName);
    void onConnectionChanged(const QString& vnaModel,
                             const QString& vnaAddress,
                             bool vnaConnected,
                             const QString& controllerIface,
                             bool dutConnected,
                             double temperatureC,
                             bool temperatureValid);
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
    void onResumeSeries();
    void onApplyVnaSettings();
    void onProbeVna();
    void onProbeFinished(bool ok, const QString& idnOrError);
    void onEtaChanged(const QString& text);
    void onSweepPreview(const QVector<double>& freqGhz,
                        const QVector<double>& magDb,
                        const QVector<double>& phaseUnwrapDeg);
    void updateCycleButtons(int state);

private:
    void loadExampleDefaults();
    void refreshThemeButton();
    void scanUnfinishedSeries();
    [[nodiscard]] QString defaultDataRoot() const;

    QString m_unfinishedSeries;

    ConnectionBar* m_connections = nullptr;
    MeasureTab* m_measure = nullptr;
    CodeMatrixTab* m_matrix = nullptr;
    DataFormatsTab* m_formats = nullptr;
    QPushButton* m_wizardBtn = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_pause = nullptr;
    QPushButton* m_stop = nullptr;

    QThread* m_thread = nullptr;
    MeasureWorker* m_worker = nullptr;
    int m_state = 0;
};
