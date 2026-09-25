#pragma once

#include "DutSimulator.h"
#include "MeasurementOrchestrator.h"
#include "VnaSimulator.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QStringList>
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
    /// 0 = DutSimulator (серия), 1 = Stub (диагностика т.14), 2 = боевой (не реализован).
    enum CtrlBackend : int { CtrlSimulator = 0, CtrlStub = 1, CtrlCombat = 2 };

    explicit MeasureWorker(QObject* parent = nullptr);
    ~MeasureWorker() override;

public slots:
    void configureVna(int backend,
                      const QString& host,
                      int port,
                      const QString& comPort,
                      bool allowDirectAccess,
                      int connectTimeoutMs = 3000,
                      int sweepTimeoutMs = 30000,
                      int measureRetries = 2);
    /// DUT-UI-001: 0=DutSimulator, 1=Stub. Серия всегда на DutSimulator; Stub — только диагностика.
    void configureController(int backend,
                             const QString& host,
                             int port,
                             const QString& comPort);
    /// connect + *IDN? через оркестратор (Idle), не напрямую IVna.
    void probeVna();
    /// UI-ERR-001: имитатор — push_instrument_error + drain в GUI-очередь.
    void simulateScpiError();
    /// GAP-WIZ-001: короткий пробный съём кодов через оркестратор (Idle).
    void runProbeCodes(double fStartHz,
                       double fStopHz,
                       int points,
                       int ifbwHz,
                       double powerDbm,
                       int averages);
    void prepare(const QString& dataRoot,
                 const QString& runConfigPath,
                 const QString& attenuatorCsvPath,
                 bool forceSafeState,
                 const QString& vnaCalibrationId = QString());
    void prepareRecovery(const QString& seriesDir);
    void start();
    void pause();
    void resume();
    void stop();
    void requestMatrixSnapshot(int channel, int attCode);
    void requestRemeasure(int channel, int attCode, const QVector<int>& phases);
    void requestCellPreview(int channel, int attCode, int phase);
    /// UI-MEAS-001: один свип S11…S22 без DUT/серии (Idle/Ready).
    void measureNow(double fStartHz,
                    double fStopHz,
                    int points,
                    int ifbwHz,
                    double powerDbm,
                    int averages);
    /// CAL-UI: шаг TwoPortCalibrationStep как int (Begin…Apply).
    void calibrateTwoPort(int step);
    /// CAL-UI: шаг OnePortCalibrationStep как int + порт 1|2.
    void calibrateOnePort(int step, int port);
    void shutdown();

signals:
    void connectionChanged(const QString& vnaModel,
                           const QString& vnaAddress,
                           bool vnaConnected,
                           const QString& controllerIface,
                           bool dutConnected,
                           double temperatureC,
                           bool temperatureValid,
                           const QString& vnaSerial,
                           const QString& vnaFirmware);
    void stateChanged(int state, const QString& russianText, const QString& colorName);
    void prepareFinished(bool ok, const QString& diagnostics);
    void probeFinished(bool ok, const QString& idnOrError);
    void probeCodesFinished(bool ok, const QString& message);
    void measureNowFinished(bool ok, const QString& message);
    void calibrateTwoPortFinished(bool ok, int step, const QString& message);
    void calibrateOnePortFinished(bool ok, int step, const QString& message);
    void progressChanged(qint64 completed,
                         qint64 total,
                         int channel,
                         int attCode,
                         int phaseCode);
    void etaChanged(const QString& text);
    void sweepPreview(const QVector<double>& freqGhz,
                      const QVector<double>& magDb,
                      const QVector<double>& phaseUnwrapDeg);
    /// Полный свип S11/S21/S12/S22 (mag + unwrap phase) из lastMeasuredSweep.
    void sparamsPreview(const QVector<double>& freqGhz,
                        const QVector<double>& s11mag,
                        const QVector<double>& s11ph,
                        const QVector<double>& s21mag,
                        const QVector<double>& s21ph,
                        const QVector<double>& s12mag,
                        const QVector<double>& s12ph,
                        const QVector<double>& s22mag,
                        const QVector<double>& s22ph);
    void cellSweepPreview(const QVector<double>& freqGhz,
                          const QVector<double>& magDb,
                          const QVector<double>& phaseUnwrapDeg);
    /// UTC последнего STATE_OK слота. Пустая строка — события нет.
    void cellSlotRecordedUtc(int channel, int attCode, int phase, const QString& timestampUtc);
    void pathsChanged(const QString& seriesRoot,
                      const QString& runConfig,
                      const QString& attenuatorCsv,
                      const QString& rawS21,
                      const QString& directLut,
                      const QString& inverseLut,
                      const QString& report,
                      const QString& manifest,
                      const QString& runEvents);
    /// Путь, число valid/total и фрагмент с диска после Complete.
    /// valid/total < 0 — файл не прочитан. Для манифеста счётчик — число строк.
    /// *Flat — mag≈0 / s21_re≈1 (типичный SIM).
    void seriesArtifactsPreview(const QString& runId,
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
    /// PLOT-010: mag + phase_error первой valid-строки прямой LUT (парсинг в worker).
    void directLutCurvePreview(const QVector<double>& freqGhz,
                               const QVector<double>& magDb,
                               const QVector<double>& phaseErrorDeg,
                               int channel,
                               int attCode,
                               int phaseCode);
    void matrixSnapshot(int channel,
                        int attCode,
                        int measuringPhase,
                        const QVector<int>& statuses,
                        const QVector<int>& attempts,
                        const QVector<int>& overloadFlags);
    void axesChanged(const QVector<int>& channels, const QVector<int>& attCodes);
    void diagnostic(const QString& text);
    /// UI-ERR-001: строки SYST:ERR? после probe/measure/drain (код, текст; команда если есть).
    void scpiErrorsReceived(const QStringList& entries);
    void finishedClean();

private slots:
    void onTick();

private:
    void rebuildVna();
    [[nodiscard]] IVna* activeVna();
    void emitPendingScpiErrors();
    void emitConnection();
    void emitState();
    void emitProgress();
    void emitPaths();
    void emitArtifactPreviews();
    void emitMatrix(int channel, int attCode);
    void maybeEmitSweepPreview();
    void emitSweepPreview(const ComplexSweep& sweep);
    void updateEta(qint64 completed, qint64 total);
    void resetEta();
    static QString formatEtaSeconds(qint64 totalSec);
    static QString stateToRussian(afar::RunState state);
    static QString stateColor(afar::RunState state);

    int m_backend{BackendSimulator};
    QString m_host{QStringLiteral("127.0.0.1")};
    int m_port{5025};
    QString m_comPort{QStringLiteral("COM3")};
    bool m_allowDirect{false};
    int m_connectTimeoutMs{3000};
    int m_sweepTimeoutMs{30000};
    int m_measureRetries{2};

    int m_ctrlBackend{CtrlSimulator};
    QString m_dutHost{QStringLiteral("192.168.0.10")};
    int m_dutPort{4001};
    QString m_dutComPort{QStringLiteral("COM4")};

    /// Полный *IDN? после успешного probe (для полосы: model/SN/FW).
    QString m_lastIdn;

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
    bool m_artifactPreviewSent = false;
};
