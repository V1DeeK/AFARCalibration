#pragma once

#include "PsProtocol.h"

#include <QElapsedTimer>
#include <QHash>
#include <QMetaType>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

Q_DECLARE_METATYPE(afar::ps::Envelope)
Q_DECLARE_METATYPE(afar::ps::Status)
Q_DECLARE_METATYPE(afar::ps::Telemetry)

/// Связь с PS изделия: TCP-канал управления (NDJSON) и приём UDP-телеметрии.
/// Контракт: docs/contracts/ps-json-protocol.md, §1–2, §5.
/// Работает асинхронно в потоке владельца, GUI не блокирует.
class PsLink final : public QObject {
    Q_OBJECT

public:
    enum class State { Disconnected, Connecting, Handshaking, Ready };
    Q_ENUM(State)

    struct Timeouts {
        int keepAliveIdleMs = 5000;  ///< keep_alive при отсутствии обмена
        int commandMs = 2000;        ///< ответ на команду
        /// Ответ на set_start_test приходит после самопроверки (v8 п. 3.2); её длительность
        /// документ не задаёт.
        int selfTestMs = 60000;
        int connectMs = 3000;        ///< HW-VNA-04 по аналогии: 3 с на соединение
        int reconnectMs = 3000;
    };

    explicit PsLink(QObject* parent = nullptr);
    ~PsLink() override;

    void setTimeouts(const Timeouts& t) { m_timeouts = t; }
    [[nodiscard]] const Timeouts& timeouts() const { return m_timeouts; }
    /// Автопереподключение только для связи, которая уже была установлена (Ready):
    /// первая неудачная попытка заканчивается ошибкой — дальше решает вызывающий
    /// (поиск платы по MAC, правка адреса оператором).
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    /// TCP к host:port, затем hello с адресом доставки телеметрии.
    /// telemetryPort = 0 — любой свободный порт (для тестов).
    void connectToPs(const QString& host,
                     quint16 port = afar::ps::kDefaultControlPort,
                     quint16 telemetryPort = afar::ps::kDefaultTelemetryPort);
    void disconnectFromPs();

    [[nodiscard]] State state() const { return m_state; }
    [[nodiscard]] bool isReady() const { return m_state == State::Ready; }
    /// Идёт попытка соединения или ждём очередную после обрыва установленной связи.
    [[nodiscard]] bool isTrying() const { return m_state != State::Disconnected || m_reconnect.isActive(); }
    [[nodiscard]] QString host() const { return m_host; }
    [[nodiscard]] quint16 port() const { return m_port; }
    /// Фактический локальный порт телеметрии; 0 — не открыт.
    [[nodiscard]] quint16 telemetryPort() const;
    [[nodiscard]] QString lastError() const { return m_lastError; }
    [[nodiscard]] QString peerVersion() const { return m_peerVersion; }

    /// Отправить команду. msg_count или -1, если связь не готова.
    int sendCommand(std::string_view msgType,
                    const nlohmann::json& payload = nlohmann::json::object());
    [[nodiscard]] int pendingCount() const { return static_cast<int>(m_pending.size()); }

    [[nodiscard]] quint64 telemetryPackets() const { return m_tlmReceived; }
    [[nodiscard]] quint64 telemetryRejected() const { return m_tlmRejected; }

signals:
    void stateChanged(PsLink::State state);
    /// Ответ на нашу команду; keep_alive сюда не попадает.
    void responseReceived(const afar::ps::Envelope& response, afar::ps::Status status);
    void commandTimedOut(const QString& msgType, int msgCount);
    void eventReceived(const afar::ps::Envelope& event);
    void telemetryReceived(const afar::ps::Telemetry& telemetry);
    /// Кадры для журнала обмена: channel — "TCP" или "UDP".
    void frameLogged(bool outgoing, const QString& channel, const QString& line);
    void errorOccurred(const QString& message);

public:
    /// Сколько датаграмм UDP разбирается за один проход цикла событий.
    static constexpr int kUdpPerPass = 200;
    /// Минимальный интервал между сигналами telemetryReceived.
    static constexpr qint64 kTlmEmitMinMs = 20;

private:
    struct Pending {
        QString msgType;
        int msgCount = 0;
        qint64 deadlineMs = 0;
    };

    void setState(State s);
    void startConnect();
    void dropLink(const QString& reason);
    void writeEnvelope(const afar::ps::Envelope& env);
    int queueCommand(std::string_view msgType, const nlohmann::json& payload);
    void onConnected();
    void onReadyRead();
    void onSocketError();
    void onDisconnected();
    void onUdpReadyRead();
    void onTick();
    void handleLine(const std::string& line);
    void handleResponse(const afar::ps::Envelope& env);
    bool bindTelemetry();
    [[nodiscard]] bool keepAlivePending() const;
    static QString pendingKey(const QString& msgType, int count);

    QTcpSocket m_socket;
    QUdpSocket m_udp;
    QTimer m_tick;
    QTimer m_reconnect;
    QElapsedTimer m_clock;
    qint64 m_lastTlmEmitMs = -1000;
    Timeouts m_timeouts;
    State m_state = State::Disconnected;
    QString m_host;
    quint16 m_port = afar::ps::kDefaultControlPort;
    quint16 m_requestedTelemetryPort = afar::ps::kDefaultTelemetryPort;
    QString m_lastError;
    QString m_peerVersion;
    bool m_wanted = false;
    bool m_autoReconnect = true;
    bool m_reachedReady = false;  ///< с последнего connectToPs() связь хоть раз стала Ready
    afar::ps::LineSplitter m_splitter;
    quint8 m_txCount = 0;
    QHash<QString, Pending> m_pending;
    qint64 m_lastRxMs = 0;
    qint64 m_lastTxMs = 0;
    qint64 m_connectStartMs = 0;
    quint64 m_tlmReceived = 0;
    quint64 m_tlmRejected = 0;
};
