#pragma once

#include "HeterTypes.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QStringList>

#include <functional>

class QSerialPort;
class QTimer;

/// Ответ платы PS на один кадр RS-485. Пустой error — байты slave получены.
struct HeterWireReply {
    QByteArray bytes;
    QString error;
};

/// Проверка файла .bin перед записью. address — куда класть образ через ST-Link.
struct HeterFirmwareInfo {
    QByteArray image;
    quint32 address = 0;
    bool application = false;
    QString summary;
    QString error;
};

/// Связь с платой гетеродина: напрямую по USB-RS485, 115200 8N1, либо через плату PS
/// командами get_lo_params и set_lo_params. Живёт в своём потоке. Слоты вызываются
/// очередью, transact() — из потока измерения.
class HeterSession final : public QObject {
    Q_OBJECT

public:
    explicit HeterSession(QObject* parent = nullptr);
    ~HeterSession() override;

    [[nodiscard]] static bool serialSupported();
    [[nodiscard]] static HeterFirmwareInfo inspectFirmware(const QByteArray& fileBytes);
    /// Только адаптеры USB-RS485. На macOS это устройство cu.*, без Bluetooth и tty.*.
    [[nodiscard]] static QList<HeterPortChoice> ports();
    [[nodiscard]] HeterState state() const;
    [[nodiscard]] bool isOpen() const;

    /// Блокирует вызывающий поток, пока кадр не уйдёт из потока сессии.
    HeterReply transact(quint8 func, const QByteArray& data);
    /// Кадр уходит функцией wire на потоке host (плата PS). host не переносить в поток сессии.
    void setBoardWire(QObject* host, std::function<HeterWireReply(const QByteArray&, int)> wire);

public slots:
    void openSerial(const QString& portName, int addr);
    void openViaBoard(int addr);
    void closeLink();
    void setFrequencyMhz(double mhz);
    void setConfig(int refExternal, int pwrA, int pwrB, bool enA, bool enB, bool enRf, bool enSyn);
    void setAttenuators(double att1Db, double att2Db);
    void setChargePump(int code);
    void queryParams();
    void queryHealth();
    /// Рабочая прошивка (не загрузчик) через загрузчик платы, команды 0x08…0x0B.
    void flashRs485(const QByteArray& image);
    /// Файл .bin через ST-Link V2. address — 0x08001000 для приложения, 0x08000000 для загрузчика.
    void flashStlink(const QString& path, quint32 address);

signals:
    void stateUpdated(const HeterState& state);
    void commandDone(const QString& op, const HeterReply& reply);
    void logLine(const QString& line);
    void linkFailed(const QString& reason);
    void flashProgress(int percent, const QString& line);
    void flashFinished(bool ok, const QString& text);

private slots:
    void poll();

private:
    HeterReply transactOnThread(quint8 func, const QByteArray& data, bool logFrames, int timeoutMs = 500);
    HeterReply transactViaBoard(quint8 func, const QByteArray& tx, bool logFrames, int timeoutMs);
    HeterWireReply askBoard(const QByteArray& frame, int timeoutMs);
    void releasePort(bool notify);
    void publish(const HeterState& state);
    void note(const QString& line);
    bool bootloaderReply(const QByteArray& payload) const;

    mutable QMutex m_mutex;
    HeterState m_state;
    QSerialPort* m_port = nullptr;
    QObject* m_wireHost = nullptr;
    std::function<HeterWireReply(const QByteArray&, int)> m_wire;
    bool m_viaBoard = false;
    QTimer* m_poll = nullptr;
    QElapsedTimer m_clock;
    int m_addr = 1;
    bool m_open = false;
    bool m_bootRecovery = false;
    QString m_portName;
};
