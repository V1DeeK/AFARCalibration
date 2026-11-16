#pragma once

#include "VideoProto.h"

#include <QElapsedTimer>
#include <QObject>

#include <atomic>
#include <functional>

class QSerialPort;
class QTimer;

/// Мастер Modbus RTU узла VIDEO. Живёт в своём потоке: обмен блокирующий, GUI не ждёт.
/// Длинные операции (проверка платы, прошивка, обзор) идут в этом же потоке и прерываются abort().
/// Без запросов дольше 1 с узел сам уходит в SAFE, поэтому опрос статуса идёт каждые 300 мс.
class VideoSession final : public QObject {
    Q_OBJECT

public:
    explicit VideoSession(QObject* parent = nullptr);
    ~VideoSession() override;

    [[nodiscard]] static bool serialSupported();
    /// Прервать идущую длинную операцию. Можно звать из любого потока.
    void abort() { m_abort.store(true); }
    void setLogPoll(bool on) { m_logPoll.store(on); }
    void setPollEnabled(bool on) { m_pollEnabled.store(on); }

public slots:
    void openPort(const QString& portName, int addr);
    void closePort();
    void apply(const video::ApplyRequest& request);
    void safe();
    void clearFault();
    void writeChargePump(int code);
    void writeRtcPaths(int value);
    void readRegisters();
    void osdWrite(int standard, bool enable, int row, int col, const QList<quint16>& glyphs, int attr);
    void runTest(const video::TestOptions& options);
    void scanChannels(int agcMode, int attenCode, int targetMv, int power);
    void flash(const QByteArray& image, quint32 version);
    void recover();

signals:
    void statusUpdated(const video::Status& status);
    /// kind: "off", "on", "boot", "err".
    void linkChanged(const QString& kind, const QString& text);
    void opDone(const QString& op, bool ok, const QString& text);
    void logLine(const QString& kind, const QString& text);
    void registersRead(const QList<quint32>& words);
    void testStep(const QString& name, const QString& result, const QString& text);
    void testFinished(const QString& verdict, int fails, int warns, const video::Status& identity);
    void scanRow(const QString& name, int mhz, int rssiMv);
    void scanFinished(int bestMhz, const QString& text);
    void flashProgress(double fraction, const QString& text);
    void flashFinished(bool ok, const QString& text);
    void statsChanged(int ok, int errors);

private slots:
    void poll();

private:
    struct Options {
        int timeoutMs = 150;
        int retries = 1;
        bool quiet = false;
    };
    QList<quint16> exchange(const QByteArray& frame, const Options& opt);
    QList<quint16> input(quint16 reg, quint16 count, const Options& opt);
    QList<quint16> input(quint16 reg, quint16 count);
    void writeMulti(quint16 reg, const QList<quint16>& values, const Options& opt);
    void writeMulti(quint16 reg, const QList<quint16>& values);
    video::Status readStatus(bool quiet = true);
    using Group = QPair<quint16, QList<quint16>>;
    video::Status transaction(quint16 cmd, const QList<Group>& groups, int timeoutMs = 1000);
    video::Status applySynth(const video::ApplyRequest& request);
    video::Status settled(int ms);
    void pause(int ms);
    void runOp(const QString& op, const std::function<QString()>& body);
    void waitBoot(int ms);
    void waitFwState(int want, int ms);
    video::Status flashImage(const QByteArray& image, quint32 version);

    QSerialPort* m_port = nullptr;
    QTimer* m_poll = nullptr;
    quint8 m_addr = 3;
    quint32 m_seq = 0;
    int m_ok = 0;
    int m_errors = 0;
    int m_pollFails = 0;
    QElapsedTimer m_lastTx;
    std::atomic<bool> m_abort{false};
    std::atomic<bool> m_logPoll{false};
    std::atomic<bool> m_pollEnabled{true};
};
