#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <deque>
#include <functional>

class QSerialPort;

/// Связь с платой ППКХ4 по виртуальному COM-порту (USB CDC, VID 0483 / PID 5740).
/// Текстовые команды по строке, ответ — строка «OK …» или «ERR …».
/// Контракт: docs/contracts/ppkh4-usb-cdc.md. Работает асинхронно, GUI не блокирует.
/// Без модуля Qt Serial Port собирается, но open() сразу сообщает об ошибке.
class Ppkh4Link final : public QObject {
    Q_OBJECT

public:
    static constexpr quint16 kVendorId = 0x0483;
    static constexpr quint16 kProductId = 0x5740;
    static constexpr int kCommandTimeoutMs = 2000;

    struct PortInfo {
        QString name;         ///< COM5, /dev/cu.usbmodem…, /dev/ttyACM0
        QString description;
        bool board = false;   ///< VID/PID платы ППКХ4
    };

    using Reply = std::function<void(const QString& line)>;

    explicit Ppkh4Link(QObject* parent = nullptr);
    ~Ppkh4Link() override;

    [[nodiscard]] static bool serialSupported();
    /// Порты системы; плата ППКХ4 — первой.
    [[nodiscard]] static QList<PortInfo> availablePorts();

    bool open(const QString& portName);
    void close();
    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] QString portName() const { return m_portName; }
    [[nodiscard]] QString lastError() const { return m_lastError; }

    /// Команды идут по одной: следующая уходит после ответа или тайм-аута 2 с.
    /// Без связи reply сразу получает «ERR not connected».
    void send(const QString& command, Reply reply = {});
    [[nodiscard]] int pendingCount() const { return static_cast<int>(m_queue.size()); }

signals:
    void opened();
    void closed(const QString& reason);
    void commandSent(const QString& command);
    void lineReceived(const QString& line);
    /// Плата ответила «ERR …»; текст после «ERR ».
    void boardError(const QString& text);

private:
    struct Job {
        QString command;
        Reply reply;
    };

    void onReadyRead();
    void onLine(const QString& line);
    void startNext();
    void finishCurrent(const QString& line);
    void dropAll(const QString& line);

    QSerialPort* m_port = nullptr;
    QTimer m_timeout;
    QByteArray m_rx;
    std::deque<Job> m_queue;
    bool m_busy = false;
    QString m_portName;
    QString m_lastError;
};
