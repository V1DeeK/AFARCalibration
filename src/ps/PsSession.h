#pragma once

#include "PsLink.h"
#include "PsProtocol.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>

#include <map>
#include <optional>

class MacResolver;

/// Одна связь с PS на всю программу: её делят вкладки «Настройки»,
/// «Тестовый прогон» и «Приёмка». Хранит адрес, MAC и
/// порты в настройках, ищет плату по MAC, если адрес не ответил (IP платы
/// меняется, MAC постоянный), ведёт журнал обмена, режим изделия, последнюю
/// телеметрию и результаты полного сканирования. Команды железу шлёт PS.
class PsSession final : public QObject {
    Q_OBJECT

public:
    struct LogLine {
        QDateTime time;
        bool outgoing = false;
        QString channel;  ///< "TCP" или "UDP"
        QString line;
    };

    /// MAC платы по умолчанию — тот же, что у РМО и у чтения параметров libiio.
    static constexpr const char* kDefaultMac = "42:7E:D7:4E:0F:77";
    static constexpr int kLogLimit = 5000;
    static constexpr int kTcpLogPerSec = 400;
    static constexpr int kUdpLogPerSec = 25;

    explicit PsSession(QObject* parent = nullptr);
    ~PsSession() override;

    [[nodiscard]] PsLink* link() const { return m_link; }

    [[nodiscard]] QString host() const;
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] quint16 telemetryPort() const;
    [[nodiscard]] QString mac() const;
    /// Через сколько мс простоя канала уходит keep_alive (по умолчанию 5000).
    [[nodiscard]] int keepAliveMs() const;
    static constexpr int kKeepAliveMinMs = 200;
    static constexpr int kKeepAliveMaxMs = 60000;
    static constexpr int kKeepAliveDefaultMs = 5000;
    /// Оператор подключался и не нажимал «Отключиться» — подключаться при запуске.
    [[nodiscard]] bool autoConnect() const;

    /// Подключиться по адресу; пустой адрес и верный MAC — сразу поиск по MAC.
    /// false и lastError() — параметры неверны или идёт серия.
    bool connectTo(const QString& host, quint16 port, quint16 telemetryPort, const QString& mac);
    /// Найти плату по MAC и подключиться к найденному адресу.
    bool findByMac(const QString& mac, quint16 port, quint16 telemetryPort);
    void disconnectFromPs();
    /// Период keep_alive; запоминается и сразу применяется к связи.
    void setKeepAliveMs(int ms);
    /// При запуске программы: подключиться, если оператор не отключался.
    void connectFromSettings();

    [[nodiscard]] bool searching() const;
    /// Идёт попытка подключения, поиск платы по MAC или ожидание переподключения.
    [[nodiscard]] bool busy() const;
    /// Для автотестов: подмена ARP и списка адресов опроса.
    [[nodiscard]] MacResolver* macResolver() const { return m_resolver; }
    [[nodiscard]] QString lastError() const { return m_lastError; }
    /// Состояние связи одной строкой для окошка «Подключение».
    [[nodiscard]] QString statusText() const;

    /// Серия калибровки занимает PS через свой контроллер: связь вкладок
    /// закрывается и после серии восстанавливается (ТЗ т. 8.2).
    void setSeriesActive(bool active);
    [[nodiscard]] bool seriesActive() const { return m_seriesActive; }
    /// Автоматическая приёмка шлёт команды сама: ручные мешали бы её шагам.
    void setCheckRunning(bool running);
    [[nodiscard]] bool checkRunning() const { return m_checkRunning; }
    /// Связь готова, серии и приёмки нет — ручные команды разрешены.
    [[nodiscard]] bool manualAllowed() const;

    /// Отправить команду. msg_count или -1 (причина — lastError() и message()).
    int send(std::string_view msgType, const nlohmann::json& payload = nlohmann::json::object());

    [[nodiscard]] bool modeKnown() const { return m_modeKnown; }
    [[nodiscard]] afar::ps::Mode mode() const { return m_mode; }
    [[nodiscard]] QString state() const { return m_state; }
    [[nodiscard]] const std::optional<afar::ps::Telemetry>& telemetry() const { return m_telemetry; }

    [[nodiscard]] const QVector<LogLine>& log() const { return m_log; }
    void clearLog();

    [[nodiscard]] const std::map<std::int64_t, afar::ps::SffResult>& sffResults() const
    {
        return m_sffResults;
    }
    [[nodiscard]] const std::optional<afar::ps::SffParams>& sffParams() const { return m_sffParams; }
    void clearSffResults();

signals:
    /// Сменилось состояние связи или поиска по MAC.
    void statusChanged();
    /// Сменились режим или состояние изделия.
    void modeChanged();
    void telemetryChanged();
    void frameAppended(const PsSession::LogLine& line);
    void logCleared();
    /// Сообщение программы: ошибка связи, отказ PS, нет ответа.
    void message(const QString& text);
    /// Сменилось, можно ли слать ручные команды.
    void controlsChanged();
    void sffChanged();
    /// Плата найдена по MAC по новому адресу.
    void hostResolved(const QString& ip);
    /// MAC платы, с которой установлена связь, определён по таблице ARP и отличался от сохранённого.
    void boardMacDetected(const QString& mac);
    /// Поиск по MAC не нашёл плату, но порт PS открыт ровно у одного узла: вероятно, это она.
    void boardCandidate(const QString& ip, const QString& mac);

private:
    void saveSettings(const QString& host, quint16 port, quint16 telemetryPort, const QString& mac);
    void startLink(const QString& host, quint16 port, quint16 telemetryPort);
    void searchByMac();
    void onLinkState(PsLink::State state);
    void onResponse(const afar::ps::Envelope& response, afar::ps::Status status);
    void onEvent(const afar::ps::Envelope& event);
    void onTelemetry(const afar::ps::Telemetry& telemetry);
    void onFrame(bool outgoing, const QString& channel, const QString& line);
    void appendLog(const LogLine& line);
    void setMode(afar::ps::Mode mode, const QString& state);
    void refuse(const QString& text);

    PsLink* m_link = nullptr;
    MacResolver* m_resolver = nullptr;
    QString m_lastError;
    QString m_searchText;
    bool m_macSearchDone = false;
    bool m_seriesActive = false;
    bool m_reconnectAfterSeries = false;
    bool m_checkRunning = false;
    bool m_modeKnown = false;
    afar::ps::Mode m_mode = afar::ps::Mode::Idle;
    QString m_state;
    std::optional<afar::ps::Telemetry> m_telemetry;
    QVector<LogLine> m_log;
    QElapsedTimer m_logClock;
    qint64 m_logWindowStartMs = 0;
    int m_logTcpInWindow = 0;
    int m_logUdpInWindow = 0;
    int m_logSkipped = 0;
    std::map<std::int64_t, afar::ps::SffResult> m_sffResults;
    std::optional<afar::ps::SffParams> m_sffParams;
};

Q_DECLARE_METATYPE(PsSession::LogLine)
