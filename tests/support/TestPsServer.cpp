#include "TestPsServer.h"

#include <QDateTime>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>

namespace ps = afar::ps;
using nlohmann::json;

TestPsServer::TestPsServer(QObject* parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_udp(this)
    , m_tlmTimer(this)
{
    m_startMs = QDateTime::currentMSecsSinceEpoch();
    resetService();
    m_tlmTimer.setInterval(1000);
    connect(m_server, &QTcpServer::newConnection, this, &TestPsServer::onNewConnection);
    connect(&m_tlmTimer, &QTimer::timeout, this, &TestPsServer::sendTelemetry);
}

TestPsServer::~TestPsServer()
{
    close();
}

bool TestPsServer::listen(quint16 port)
{
    return m_server->listen(QHostAddress::LocalHost, port);
}

void TestPsServer::close()
{
    m_tlmTimer.stop();
    if (m_client) {
        m_client->abort();
        m_client->deleteLater();
    }
    m_server->close();
}

quint16 TestPsServer::port() const
{
    return m_server->serverPort();
}

bool TestPsServer::isListening() const
{
    return m_server->isListening();
}

void TestPsServer::setFault(bool fault)
{
    m_fault = fault;
    if (fault) {
        m_mode = ps::Mode::Idle;
    }
}

void TestPsServer::setTelemetryPeriodMs(int ms)
{
    m_tlmOff = ms <= 0;
    if (m_tlmOff) {
        m_tlmTimer.stop();
        return;
    }
    m_tlmTimer.setInterval(ms);
}

void TestPsServer::resetService()
{
    const json lo{{"pwr_b", -4}, {"pwr_a", -4}, {"en_b", 0}, {"en_a", 0}, {"en_rf", 0},
                  {"en_syn", 0}, {"ref", 0}, {"freq", 0}, {"att1", 31.5}, {"att2", 31.5},
                  {"temp", 41.5}, {"lock", 0}};
    m_video = lo;
    m_lo1 = lo;
    m_lo1["addr"] = ps::kLoMixer;
    m_lo2 = lo;
    m_lo2["addr"] = ps::kLoCalibration;
    m_video["att_mode"] = 0;
    m_video["en_vid"] = 0;
    m_video["temp"] = 44.0;
    m_mixer = json{{"ch", json::array()}, {"temp", 39.0}};
    for (int n = 1; n <= ps::kMixerChannels; ++n) {
        m_mixer["ch"].push_back(json{{"n", n}, {"en", 0}, {"rp", 0}, {"if_att", 0.0}, {"att", 31.5}});
    }
    for (int i = 0; i < ps::kChannels; ++i) {
        m_codes[static_cast<std::size_t>(i)] = {i + 1, 0, 0};
    }
    m_beamSrc = "NONE";
}

void TestPsServer::onNewConnection()
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
        connect(next, &QTcpSocket::readyRead, this, &TestPsServer::onReadyRead);
        connect(next, &QTcpSocket::disconnected, this, [this, next] {
            if (m_client == next) {
                m_tlmTimer.stop();
            }
            next->deleteLater();
        });
    }
}

void TestPsServer::onReadyRead()
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

void TestPsServer::reply(const ps::Envelope& cmd, ps::Status status, json payload)
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

void TestPsServer::handleSff(const ps::Envelope& cmd)
{
    const std::string& t = cmd.msg_type;
    if (m_mode != ps::Mode::FullScanFreq) {
        reply(cmd, ps::Status::NotSupported);
        return;
    }
    if (t == ps::msg::SetSffParameters) {
        json merged = m_sff.toJson();
        merged.update(cmd.payload);
        ps::SffParams p;
        if (!ps::SffParams::fromJson(merged, p)) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_sff = p;
        reply(cmd, ps::Status::Ok, m_sff.toJson());
        return;
    }
    if (t == ps::msg::GetSffParameters) {
        reply(cmd, ps::Status::Ok, m_sff.toJson());
        return;
    }
    reply(cmd, ps::Status::Ok);
    if (t == ps::msg::SetSffStart) {
        // Один обзор: три частоты, на каждой — три азимута сектора.
        for (int i = 0; i < 3; ++i) {
            const std::int64_t freq = m_sff.freq_min + (m_sff.freq_max - m_sff.freq_min) * i / 2;
            json fft = json::array();
            for (int a = 0; a < 3; ++a) {
                const double az = m_sff.az_min + (m_sff.az_max - m_sff.az_min) * a / 2.0;
                fft.push_back(json{{"az", az}, {"pwr", json::array({50.0 + i, 60.0 + a, 55.0})}});
            }
            ps::Envelope ev;
            ev.msg_type = std::string(ps::msg::SendSffResult);
            ev.msg_class = ps::MsgClass::Event;
            ev.msg_count = m_eventCount++;
            ev.payload = json{{"freq", freq}, {"cal", i == 0 ? "NO" : "OK"}, {"fft", fft}};
            const std::string line = ps::encodeLine(ev);
            if (m_client) {
                m_client->write(line.data(), static_cast<qint64>(line.size()));
            }
        }
    }
}

json TestPsServer::channelsJson() const
{
    json payload;
    ps::channelsToJson(std::vector<ps::ChannelCodes>(m_codes.begin(), m_codes.end()), payload);
    return payload;
}

void TestPsServer::handle(const ps::Envelope& cmd)
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
        if (!m_tlmOff) {
            m_tlmTimer.start();
        }
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

    if (t == ps::msg::SetSffParameters || t == ps::msg::GetSffParameters || t == ps::msg::SetSffStart ||
        t == ps::msg::SetSffStop) {
        handleSff(cmd);
        return;
    }

    const bool serviceCommand =
        t == ps::msg::SetPhaseAtten || t == ps::msg::GetPhaseAtten ||
        t == ps::msg::SetBeamAzimuth || t == ps::msg::SetBeam ||
        t == ps::msg::GetBeam || t == ps::msg::Rs485Send || t == ps::msg::SetLo ||
        t == ps::msg::GetLo || t == ps::msg::SetMixer || t == ps::msg::GetMixer ||
        t == ps::msg::SetVideo || t == ps::msg::GetVideo || t == ps::msg::GetRfmTelemetry ||
        t == ps::msg::GetTrxStatus || t == ps::msg::GetPlStatus;
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
        m_beamSrc = "CODES";
        reply(cmd, ps::Status::Ok, channelsJson());
    } else if (t == ps::msg::GetPhaseAtten) {
        reply(cmd, ps::Status::Ok, channelsJson());
    } else if (t == ps::msg::GetRfmTelemetry) {
        reply(cmd, ps::Status::Ok, json{{"temp", json::array({34.1, 35.2, 36.3, 37.4})}});
    } else if (t == ps::msg::GetTrxStatus) {
        reply(cmd, ps::Status::Ok, json{{"spi_ok", 1}, {"bbpll_lock", 1}, {"rx_lo_lock", 1},
                                         {"cal_done", 1}, {"if_ok", 1}, {"pn_err", 0},
                                         {"adc_ovr", 0}, {"dma_ovf", 0}, {"temp", 41.5}});
    } else if (t == ps::msg::GetPlStatus) {
        reply(cmd, ps::Status::Ok, json{{"adc_overrun", 0}, {"fft0_error", 0}, {"fft1_error", 0}});
    } else if (t == ps::msg::SetBeamAzimuth) {
        const auto az = in.find("az");
        if (az == in.end() || !az->is_number() || std::fabs(az->get<double>()) > ps::kAzimuthLimitDeg) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_beamSrc = "AZIMUTH";
        m_beamAz = az->get<double>();
        reply(cmd, ps::Status::Ok, json{{"az", m_beamAz}});
    } else if (t == ps::msg::SetBeam) {
        const auto beam = in.find("beam");
        if (beam == in.end() || !beam->is_number_integer() || beam->get<int>() < 0 ||
            beam->get<int>() > 63) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_beamSrc = "TABLE";
        m_beam = beam->get<int>();
        reply(cmd, ps::Status::Ok, json{{"beam", m_beam}});
    } else if (t == ps::msg::GetBeam) {
        json p{{"src", m_beamSrc}};
        if (m_beamSrc == "AZIMUTH") {
            p["az"] = m_beamAz;
        } else if (m_beamSrc == "TABLE") {
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
    } else {
        handleModule(cmd);
    }
}

namespace {

/// Поля из in по таблице fields в to. false — неизвестное поле, значение вне
/// диапазона или ни одного поля; тогда to не меняется.
bool applyFields(const json& in, const std::vector<ps::ModuleField>& fields, json& to,
                 std::initializer_list<std::string_view> skip)
{
    json next = to;
    int n = 0;
    for (const auto& [key, value] : in.items()) {
        if (std::find(skip.begin(), skip.end(), key) != skip.end()) {
            continue;
        }
        const auto f = std::find_if(fields.begin(), fields.end(),
                                    [&key](const ps::ModuleField& x) { return x.key == key; });
        if (f == fields.end() || !value.is_number() ||
            !ps::moduleValueValid(f->kind, value.get<double>())) {
            return false;
        }
        next[key] = ps::moduleFieldValue(f->kind, value.get<double>());
        ++n;
    }
    if (n == 0) {
        return false;
    }
    to = std::move(next);
    return true;
}

json withoutStatus(json j, std::initializer_list<std::string_view> drop)
{
    for (auto k : drop) {
        j.erase(std::string(k));
    }
    return j;
}

}  // namespace

const json& TestPsServer::moduleState(int addr) const
{
    return addr == ps::kLoMixer ? m_lo1 : addr == ps::kLoCalibration ? m_lo2 : m_video;
}

void TestPsServer::handleModule(const ps::Envelope& cmd)
{
    const auto& t = cmd.msg_type;
    const json& in = cmd.payload;
    if (t == ps::msg::SetLo || t == ps::msg::GetLo) {
        const auto addr = in.find("addr");
        if (addr == in.end() || !addr->is_number_integer() ||
            (*addr != ps::kLoMixer && *addr != ps::kLoCalibration)) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        if (m_modulesSilent) {
            reply(cmd, ps::Status::HwNotReady);
            return;
        }
        json& lo = *addr == ps::kLoMixer ? m_lo1 : m_lo2;
        if (t == ps::msg::GetLo) {
            reply(cmd, ps::Status::Ok, lo);
            return;
        }
        if (!applyFields(in, ps::loFields(), lo, {"addr"})) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        reply(cmd, ps::Status::Ok, withoutStatus(lo, {"temp", "lock"}));
        return;
    }
    if (m_modulesSilent) {
        reply(cmd, ps::Status::HwNotReady);
        return;
    }
    if (t == ps::msg::GetVideo) {
        reply(cmd, ps::Status::Ok, m_video);
    } else if (t == ps::msg::SetVideo) {
        if (!applyFields(in, ps::videoFields(), m_video, {})) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        reply(cmd, ps::Status::Ok, withoutStatus(m_video, {"temp", "lock"}));
    } else if (t == ps::msg::GetMixer) {
        reply(cmd, ps::Status::Ok, m_mixer);
    } else if (t == ps::msg::SetMixer) {
        const auto ch = in.find("ch");
        if (ch == in.end() || !ch->is_array() || ch->empty() ||
            ch->size() > static_cast<std::size_t>(ps::kMixerChannels)) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        json next = m_mixer;
        json answer = json::array();
        std::set<int> seen;
        for (const auto& e : *ch) {
            const auto n = e.find("n");
            if (!e.is_object() || n == e.end() || !n->is_number_integer() || *n < 1 ||
                *n > ps::kMixerChannels || !seen.insert(n->get<int>()).second) {
                reply(cmd, ps::Status::InvalidArgument);
                return;
            }
            json& slot = next["ch"][static_cast<std::size_t>(n->get<int>() - 1)];
            if (!applyFields(e, ps::mixerChannelFields(), slot, {"n"})) {
                reply(cmd, ps::Status::InvalidArgument);
                return;
            }
            answer.push_back(slot);
        }
        m_mixer = std::move(next);
        reply(cmd, ps::Status::Ok, json{{"ch", answer}});
    }
}

void TestPsServer::sendTelemetry()
{
    if (!m_helloDone || m_tlmPort == 0 || m_tlmOff) {
        return;
    }
    QByteArray packet(10, char(0xff));
    if (m_fault) {
        packet[9] = char(0x7f); // бит 79: самопроверка калибровочных таблиц
    }
    m_udp.writeDatagram(packet, QHostAddress(m_tlmHost), m_tlmPort);
}
