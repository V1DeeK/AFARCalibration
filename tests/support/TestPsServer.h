#pragma once

#include "PsProtocol.h"

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUdpSocket>

#include <array>
#include <string_view>
#include <vector>

class QTcpServer;
class QTcpSocket;

/// Тестовый сервер на localhost для автотестов TCP и UDP. Только в tests/:
/// в программу не входит, PS изделия делает разработчик PS. Отвечает по
/// контракту docs/contracts/ps-json-protocol.md: hello, keep_alive,
/// set_mode/get_state, полное сканирование (§4.6), оперативный режим (v8 п. 3.5), сервисные команды v3 (с модулями §4.5), UDP-телеметрия.
/// Одно соединение; новое закрывает старое.
class TestPsServer final : public QObject {
    Q_OBJECT

public:
    explicit TestPsServer(QObject* parent = nullptr);
    ~TestPsServer() override;

    /// Слушать 127.0.0.1:port (0 — любой свободный). false — порт занят.
    bool listen(quint16 port = 0);
    void close();
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] bool isListening() const;

    /// Сломать поведение для тестов отказов.
    void setSilent(bool silent) { m_silent = silent; }
    void setFault(bool fault);
    /// 0 — телеметрию не слать совсем.
    void setTelemetryPeriodMs(int ms);

    [[nodiscard]] afar::ps::Mode mode() const { return m_mode; }
    [[nodiscard]] const std::array<afar::ps::ChannelCodes, afar::ps::kChannels>& codes() const
    {
        return m_codes;
    }
    [[nodiscard]] int commandsReceived() const { return m_commands; }
    /// Все принятые команды в порядке прихода.
    [[nodiscard]] const std::vector<afar::ps::Envelope>& received() const { return m_received; }
    /// Последняя команда этого типа; nullptr — не приходила.
    [[nodiscard]] const afar::ps::Envelope* last(std::string_view msgType) const
    {
        for (auto it = m_received.rbegin(); it != m_received.rend(); ++it) {
            if (it->msg_type == msgType) {
                return &*it;
            }
        }
        return nullptr;
    }
    /// 10 байт технического состояния вместо «всё исправно»; пусто — по умолчанию.
    void setStatusBytes(const QByteArray& bytes) { m_statusBytes = bytes; }
    void sendTelemetryNow() { sendTelemetry(); }
    /// Состояние модулей: addr 1, 2 — гетеродины, 4 — видеоприёмник.
    [[nodiscard]] const nlohmann::json& moduleState(int addr) const;
    [[nodiscard]] const nlohmann::json& mixerState() const { return m_mixer; }
    [[nodiscard]] const afar::ps::SffParams& sffParams() const { return m_sff; }
    /// Параметры OSD (таблица 13) и последний отладочный текст set_osd_text; пусто после clear_osd_text.
    [[nodiscard]] const nlohmann::json& osd() const { return m_osd; }
    [[nodiscard]] const nlohmann::json& osdText() const { return m_osdText; }
    /// Параметры оперативного режима (v8 п. 3.5): freq, acq, loss, mode, az_min, az_max,
    /// az_fix, data, cal, hold; состояние вложенного КА — opState().
    [[nodiscard]] const nlohmann::json& op() const { return m_op; }
    [[nodiscard]] const std::string& opState() const { return m_opState; }
    [[nodiscard]] int selfTests() const { return m_selfTests; }
    [[nodiscard]] int resets() const { return m_resets; }
    /// Модуль не отвечает по RS-485 — HW_NOT_READY на команды §4.5.
    void setModulesSilent(bool silent) { m_modulesSilent = silent; }

private:
    void onNewConnection();
    void onReadyRead();
    void handle(const afar::ps::Envelope& cmd);
    void handleModule(const afar::ps::Envelope& cmd);
    void handleSff(const afar::ps::Envelope& cmd);
    void handleOp(const afar::ps::Envelope& cmd);
    void sendEvent(std::string_view type, nlohmann::json payload);
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
    bool m_tlmOff = false;
    QString m_tlmHost;
    quint16 m_tlmPort = 0;
    quint8 m_tlmCount = 0;
    int m_commands = 0;
    std::vector<afar::ps::Envelope> m_received;
    QByteArray m_statusBytes;
    qint64 m_startMs = 0;

    afar::ps::Mode m_mode = afar::ps::Mode::Idle;
    std::array<afar::ps::ChannelCodes, afar::ps::kChannels> m_codes{};
    std::string m_beamSrc = "NONE";
    double m_beamAz = 0.0;
    int m_beam = 0;
    nlohmann::json m_lo1;
    nlohmann::json m_lo2;
    nlohmann::json m_video;
    nlohmann::json m_mixer;  ///< {"ch":[4 канала], "temp"}
    bool m_modulesSilent = false;
    nlohmann::json m_osd = {{"en", 1}, {"std", "PAL"}, {"src", "AUTO"},
                            {"fields", {"state", "freq", "az", "cls", "snr"}}, {"hos", 0}, {"vos", 0}};
    nlohmann::json m_osdText = nlohmann::json::object();
    int m_selfTests = 0;
    int m_resets = 0;
    afar::ps::SffParams m_sff;
    quint8 m_eventCount = 0;
    std::string m_opState = "OP_INITIALIZATION";
    std::string m_sffState = "SFF_IDLE";  ///< SFF_FREQ — обзор идёт, до set_sff_stop
    nlohmann::json m_op = {{"freq", 4900000000LL}, {"acq", "AUTO"}, {"loss", "AUTO"}, {"mode", "SCAN"},
                           {"az_min", -30.0}, {"az_max", 30.0}, {"az_fix", 0.0}, {"data", "AUTO"},
                           {"cal", "NO"}, {"hold", false}};
};
