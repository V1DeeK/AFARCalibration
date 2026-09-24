#pragma once

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

class C2220Vna;
class IScpiTransport;
class IVna;
class QTimer;
class ScpiComTransport;
class ScpiSocketTransport;

/// Worker измерения в QThread: prepare/start/pause/stop + stepOnce без блокировки GUI.
class MeasureWorker final : public QObject {
    Q_OBJECT

public:
    /// 0 = имитатор, 1 = TCP Socket (S2VNA), 2 = COM.
    enum VnaBackend : int { BackendSimulator = 0, BackendSocket = 1, BackendCom = 2 };

    explicit MeasureWorker(QObject* parent = nullptr);
    ~MeasureWorker() override;

public slots:
    void configureVna(int backend,
                      const QString& host,
                      int port,
                      const QString& comPort,
                      bool allowDirectAccess);
    /// connect + *IDN? (и disconnect для имитатора не обязателен).
    void probeVna();
    void measureSingleSweep(double fStartGhz,
                            double fStopGhz,
                            int points,
                            int ifbwHz,
                            double powerDbm,
                            int averages,
                            int sParameter);
    void saveLastSweepCsv(const QString& csvPath);
    void runCalibrationStep(int step,
                            double fStartGhz,
                            double fStopGhz,
                            int points,
                            int ifbwHz,
                            double powerDbm,
                            int averages);
    void prepare(const QString& dataRoot,
                 const QString& runConfigPath,
                 const QString& attenuatorCsvPath,
                 bool forceSafeState);
    void prepareRecovery(const QString& seriesDir);
    void start();
    void pause();
    void resume();
    void stop();
    void requestMatrixSnapshot(int channel, int attCode);
    void requestRemeasure(int channel, int attCode, const QVector<int>& phases);
    void requestCellPreview(int channel, int attCode, int phase);
    void shutdown();

signals:
    void connectionChanged(const QString& vnaModel,
                           const QString& vnaAddress,
                           bool vnaConnected,
                           const QString& controllerIface,
                           bool dutConnected,
                           double temperatureC,
                           bool temperatureValid);
    void stateChanged(int state, const QString& russianText, const QString& colorName);
    void prepareFinished(bool ok, const QString& diagnostics);
    void probeFinished(bool ok, const QString& idnOrError);
    void singleSweepFinished(bool ok, const QString& message);
    void csvSaveFinished(bool ok, const QString& message, const QString& csvPath);
    void calibrationFinished(bool ok, int step, const QString& message);
    void filterMetricsChanged(const QString& text);
    void progressChanged(qint64 completed,
                         qint64 total,
                         int channel,
                         int attCode,
                         int phaseCode);
    void etaChanged(const QString& text);
    void sweepPreview(const QVector<double>& freqGhz,
                      const QVector<double>& magDb,
                      const QVector<double>& phaseUnwrapDeg);
    void cellSweepPreview(const QVector<double>& freqGhz,
                          const QVector<double>& magDb,
                          const QVector<double>& phaseUnwrapDeg);
    void pathsChanged(const QString& seriesRoot,
                      const QString& runConfig,
                      const QString& attenuatorCsv,
                      const QString& rawS21,
                      const QString& directLut,
                      const QString& inverseLut,
                      const QString& report,
                      const QString& manifest,
                      const QString& runEvents);
    void matrixSnapshot(int channel,
                        int attCode,
                        int measuringPhase,
                        const QVector<int>& statuses,
                        const QVector<int>& attempts,
                        const QVector<int>& overloadFlags);
    void axesChanged(const QVector<int>& channels, const QVector<int>& attCodes);
    void diagnostic(const QString& text);
    void finishedClean();

private slots:
    void onTick();

private:
    void rebuildVna();
    [[nodiscard]] IVna* activeVna();
    void emitConnection();
    void emitState();
    void emitProgress();
    void emitPaths();
    void emitMatrix(int channel, int attCode);
    void maybeEmitSweepPreview();
    void updateEta(qint64 completed, qint64 total);
    void resetEta();
    static QString formatEtaSeconds(qint64 totalSec);
    static QString stateToRussian(afar::RunState state);
    static QString stateColor(afar::RunState state);

    int m_backend{BackendSimulator};
    QString m_host{QStringLiteral("127.0.0.1")};
    int m_port{5025};
    QString m_comPort{QStringLiteral("COM3")};
    QString m_identifiedIdn;
    bool m_allowDirect{false};
    ComplexSweep m_lastSingleSweep;
    QString m_lastSingleParameter;

    std::unique_ptr<VnaSimulator> m_simVna;
    std::unique_ptr<ScpiSocketTransport> m_socket;
    std::unique_ptr<ScpiComTransport> m_com;
    std::unique_ptr<C2220Vna> m_c2220;
    IVna* m_vna{nullptr};

    DutSimulator m_dut;
    std::unique_ptr<afar::MeasurementOrchestrator> m_orch;
    QTimer* m_timer = nullptr;
    int m_matrixChannel = 1;
    int m_matrixAtt = 0;

    QElapsedTimer m_sweepThrottle;
    bool m_sweepThrottleArmed = false;

    QElapsedTimer m_etaTimer;
    qint64 m_etaBaseCompleted = -1;
    bool m_etaActive = false;
};
