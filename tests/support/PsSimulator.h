#pragma once

#include "PsProtocol.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUdpSocket>

#include <array>

class QTcpServer;
class QTcpSocket;

/// Ответчик PS для автотестов: в программу не входит, программа работает
/// только с PS изделия. Отвечает по контракту docs/contracts/ps-json-protocol.md:
/// hello, keep_alive, set_mode/get_state, сервисные команды, UDP-телеметрия.
/// Одно соединение; новое закрывает старое.
class PsSimulator final : public QObject {
    Q_OBJECT

public:
    explicit PsSimulator(QObject* parent = nullptr);
    ~PsSimulator() override;

    /// Слушать 127.0.0.1:port (0 — любой свободный). false — порт занят.
    bool listen(quint16 port = 0);
    void close();
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] bool isListening() const;

    /// Сломать поведение для тестов отказов.
    void setSilent(bool silent) { m_silent = silent; }
    void setFault(bool fault);
    void setTelemetryPeriodMs(int ms);

    [[nodiscard]] afar::ps::Mode mode() const { return m_mode; }
    [[nodiscard]] const std::array<afar::ps::ChannelCodes, afar::ps::kChannels>& codes() const
    {
        return m_codes;
    }
    [[nodiscard]] int commandsReceived() const { return m_commands; }

private:
    void onNewConnection();
    void onReadyRead();
    void handle(const afar::ps::Envelope& cmd);
    void reply(const afar::ps::Envelope& cmd, afar::ps::Status status,
               nlohmann::json payload = nlohmann::json::object());
    void sendTelemetry();
    [[nodiscard]] nlohmann::json channelsJson() const;
    void resetService();

    QTcpServer* m_server = nullptr;
    QPointer<QTcpSocket> m_client;
    QUdpSocket m_udp;
    QTimer m_tlmTimer;
    afar::ps::LineSplitter m_splitter;
    bool m_helloDone = false;
    bool m_silent = false;
    bool m_fault = false;
    QString m_tlmHost;
    quint16 m_tlmPort = 0;
    quint8 m_tlmCount = 0;
    int m_commands = 0;
    qint64 m_startMs = 0;

    afar::ps::Mode m_mode = afar::ps::Mode::Idle;
    std::array<afar::ps::ChannelCodes, afar::ps::kChannels> m_codes{};
    afar::ps::AdcParams m_adc{5'800'000'000LL, 20'000'000, 30'720'000, 40.0};
    std::string m_beamSrc = "none";
    double m_beamAz = 0.0;
    int m_beam = 0;
};
