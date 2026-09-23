#pragma once

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>
#include <memory>

class QTimer;

/// Worker измерения в QThread: prepare/start/pause/stop + stepOnce без блокировки GUI.
class MeasureWorker final : public QObject {
    Q_OBJECT

public:
    explicit MeasureWorker(QObject* parent = nullptr);
    ~MeasureWorker() override;

public slots:
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

    VnaSimulator m_vna;
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
