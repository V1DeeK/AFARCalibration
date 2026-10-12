#include "PsSimulator.h"

#include <QDateTime>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <cmath>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

}  // namespace

PsSimulator::PsSimulator(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_udp(this)
    , m_tlmTimer(this)
{
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    resetService();
    m_tlmTimer.setInterval(1000);
    connect(m_server, &QTcpServer::newConnection, this, &PsSimulator::onNewConnection);
    connect(&m_tlmTimer, &QTimer::timeout, this, &PsSimulator::sendTelemetry);
}

PsSimulator::~PsSimulator()
{
    close();
}

bool PsSimulator::listen(quint16 port)
{
    return m_server->listen(QHostAddress::LocalHost, port);
}

void PsSimulator::close()
{
    m_tlmTimer.stop();
    if (m_client) {
        m_client->abort();
        m_client->deleteLater();
    }
    m_server->close();
}

quint16 PsSimulator::port() const
{
    return m_server->serverPort();
}

bool PsSimulator::isListening() const
{
    return m_server->isListening();
}

void PsSimulator::setFault(bool fault)
{
    m_fault = fault;
    if (fault) {
        m_mode = ps::Mode::Idle;
    }
}

void PsSimulator::setTelemetryPeriodMs(int ms)
{
    m_tlmTimer.setInterval(ms);
}

void PsSimulator::resetService()
{
    for (int i = 0; i < ps::kChannels; ++i) {
        m_codes[static_cast<std::size_t>(i)] = {i + 1, 0, 0};
    }
    m_beamSrc = "none";
}

void PsSimulator::onNewConnection()
{
    while (QTcpSocket* next = m_server->nextPendingConnection()) {
        if (m_client) {
            m_client->abort();
            m_client->deleteLater();
        }
        m_client = next;
        m_helloDone = false;
        m_splitter.clear();
        m_tlmTimer.stop();
        connect(next, &QTcpSocket::readyRead, this, &PsSimulator::onReadyRead);
        connect(next, &QTcpSocket::disconnected, this, [this, next] {
            if (m_client == next) {
                m_tlmTimer.stop();
            }
            next->deleteLater();
        });
    }
}

void PsSimulator::onReadyRead()
{
    if (!m_client) {
        return;
    }
    const QByteArray bytes = m_client->readAll();
    if (!m_splitter.append(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())))) {
        m_client->abort();
        return;
    }
    std::string line;
    while (m_splitter.takeLine(line)) {
        ps::Envelope env;
        if (!ps::decodeLine(line, env) || env.msg_class != ps::MsgClass::Command) {
            continue;
        }
        ++m_commands;
        if (!m_silent) {
            handle(env);
        }
    }
}

void PsSimulator::reply(const ps::Envelope& cmd, ps::Status status, json payload)
{
    if (!m_client) {
        return;
    }
    ps::Envelope r;
    r.msg_type = cmd.msg_type;
    r.msg_class = ps::MsgClass::Response;
    r.msg_count = cmd.msg_count;
    json p = json{{"status", std::string(ps::toString(status))}};
    if (status == ps::Status::Ok && payload.is_object()) {
        p.update(payload);
    }
    r.payload = std::move(p);
    const std::string line = ps::encodeLine(r);
    m_client->write(line.data(), static_cast<qint64>(line.size()));
}

json PsSimulator::channelsJson() const
{
    json payload;
    ps::channelsToJson(std::vector<ps::ChannelCodes>(m_codes.begin(), m_codes.end()), payload);
    return payload;
}

void PsSimulator::handle(const ps::Envelope& cmd)
{
    const std::string& t = cmd.msg_type;
    const json& in = cmd.payload;
    if (t == ps::msg::Hello) {
        const auto ver = in.find("ver");
        const auto ip = in.find("tlm_ip");
        const auto port = in.find("tlm_port");
        if (ver == in.end() || !ver->is_string() || ip == in.end() || !ip->is_string() ||
            port == in.end() || !port->is_number_integer()) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        if (!ps::versionsCompatible(ver->get<std::string>(), ps::kProtocolVersion)) {
            reply(cmd, ps::Status::NotSupported);
            return;
        }
        m_helloDone = true;
        m_tlmHost = QString::fromStdString(ip->get<std::string>());
        m_tlmPort = static_cast<quint16>(port->get<int>());
        reply(cmd, ps::Status::Ok, json{{"ver", std::string(ps::kProtocolVersion)},
                                        {"tlm_ip", ip->get<std::string>()},
                                        {"tlm_port", m_tlmPort}});
        m_tlmTimer.start();
        sendTelemetry();
        return;
    }
    if (!m_helloDone) {
        reply(cmd, ps::Status::NotSupported);
        return;
    }
    if (t == ps::msg::KeepAlive) {
        reply(cmd, ps::Status::Ok);
        return;
    }
    if (t == ps::msg::GetState) {
        const std::string state = m_mode == ps::Mode::Idle ? (m_fault ? "FAULT" : "READY") : "";
        reply(cmd, ps::Status::Ok,
              json{{"mode", std::string(ps::toString(m_mode))}, {"state", state}});
        return;
    }
    if (t == ps::msg::SetMode) {
        ps::Mode target{};
        const auto it = in.find("mode");
        if (it == in.end() || !it->is_string() || !ps::parseMode(it->get<std::string>(), target)) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        if (m_fault) {
            reply(cmd, ps::Status::HwNotReady);
            return;
        }
        if (target == ps::Mode::Service && m_mode != ps::Mode::Idle && m_mode != ps::Mode::Service) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        if (m_mode == ps::Mode::Service && target != ps::Mode::Service) {
            resetService();
        }
        m_mode = target;
        reply(cmd, ps::Status::Ok, json{{"mode", std::string(ps::toString(m_mode))}});
        sendTelemetry();
        return;
    }

    const bool serviceCommand =
        t == ps::msg::SetPhaseAtten || t == ps::msg::GetPhaseAtten || t == ps::msg::SetAdc ||
        t == ps::msg::GetAdc || t == ps::msg::SetBeamAzimuth || t == ps::msg::SetBeam ||
        t == ps::msg::GetBeam || t == ps::msg::Rs485Send;
    if (!serviceCommand) {
        reply(cmd, ps::Status::NotSupported);
        return;
    }
    if (m_mode != ps::Mode::Service) {
        reply(cmd, ps::Status::NotSupported);
        return;
    }

    if (t == ps::msg::SetPhaseAtten) {
        std::vector<ps::ChannelCodes> ch;
        if (!ps::channelsFromJson(in, ch)) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        for (const auto& c : ch) {
            m_codes[static_cast<std::size_t>(c.n - 1)] = c;
        }
        m_beamSrc = "codes";
        reply(cmd, ps::Status::Ok, channelsJson());
    } else if (t == ps::msg::GetPhaseAtten) {
        reply(cmd, ps::Status::Ok, channelsJson());
    } else if (t == ps::msg::SetAdc) {
        ps::AdcParams adc;
        if (!ps::AdcParams::fromJson(in, adc)) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        // Сетка синтезатора 1024 Гц: фактическая lo может отличаться от заданной.
        adc.lo = adc.lo / 1024 * 1024;
        m_adc = adc;
        reply(cmd, ps::Status::Ok, m_adc.toJson());
    } else if (t == ps::msg::GetAdc) {
        reply(cmd, ps::Status::Ok, m_adc.toJson());
    } else if (t == ps::msg::SetBeamAzimuth) {
        const auto az = in.find("az");
        if (az == in.end() || !az->is_number() || std::fabs(az->get<double>()) > ps::kAzimuthLimitDeg) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_beamSrc = "azimuth";
        m_beamAz = az->get<double>();
        reply(cmd, ps::Status::Ok, json{{"az", m_beamAz}});
    } else if (t == ps::msg::SetBeam) {
        const auto beam = in.find("beam");
        if (beam == in.end() || !beam->is_number_integer() || beam->get<int>() < 0 ||
            beam->get<int>() > 63) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_beamSrc = "table";
        m_beam = beam->get<int>();
        reply(cmd, ps::Status::Ok, json{{"beam", m_beam}});
    } else if (t == ps::msg::GetBeam) {
        json p{{"src", m_beamSrc}};
        if (m_beamSrc == "azimuth") {
            p["az"] = m_beamAz;
        } else if (m_beamSrc == "table") {
            p["beam"] = m_beam;
        }
        reply(cmd, ps::Status::Ok, p);
    } else if (t == ps::msg::Rs485Send) {
        const auto data = in.find("data");
        if (data == in.end() || !data->is_string() || !ps::hexBytesValid(data->get<std::string>())) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        reply(cmd, ps::Status::Ok, json{{"data", data->get<std::string>()}});
    }
}

void PsSimulator::sendTelemetry()
{
    if (!m_helloDone || m_tlmPort == 0) {
        return;
    }
    ps::Envelope e;
    e.msg_type = std::string(ps::msg::Telemetry);
    e.msg_class = ps::MsgClass::Event;
    e.msg_count = m_tlmCount++;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const std::string state =
        m_mode == ps::Mode::Idle ? (m_fault ? "FAULT" : "READY") : std::string();
    e.payload = json{
        {"time", now},
        {"uptime", (now - m_startMs) / 1000},
        {"mode", std::string(ps::toString(m_mode))},
        {"state", state},
        {"devices",
         json{{"power", "OK"}, {"temperature", "OK"}, {"pl", "OK"}, {"synth", "OK"},
              {"adc", "OK"}, {"beamformer", m_fault ? "FAIL" : "OK"}, {"calibration", "NOT_CHECKED"},
              {"rs485", "OK"}}},
        {"temperature", json{{"pl", 48.5}, {"rf", 36.0}}},
        {"power", json{{"v12", 12.02}, {"v5", 5.01}}},
    };
    const std::string dg = ps::encodeDatagram(e);
    m_udp.writeDatagram(dg.data(), static_cast<qint64>(dg.size()), QHostAddress(m_tlmHost), m_tlmPort);
}
