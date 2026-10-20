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
    m_video["pres"] = 1;
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
        m_received.push_back(env);
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
    m_sffState = t == ps::msg::SetSffStart ? "SFF_FREQ" : "SFF_IDLE";
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

void TestPsServer::sendEvent(std::string_view type, json payload)
{
    ps::Envelope ev;
    ev.msg_type = std::string(type);
    ev.msg_class = ps::MsgClass::Event;
    ev.msg_count = m_eventCount++;
    ev.payload = std::move(payload);
    const std::string line = ps::encodeLine(ev);
    if (m_client) {
        m_client->write(line.data(), static_cast<qint64>(line.size()));
    }
}

void TestPsServer::handleOp(const ps::Envelope& cmd)
{
    const std::string& t = cmd.msg_type;
    const json& in = cmd.payload;
    if (m_mode != ps::Mode::Operational) {
        reply(cmd, ps::Status::NotSupported);
        return;
    }
    const auto azOk = [](const json& o, const char* key) {
        const auto it = o.find(key);
        return it == o.end() || (it->is_number() && std::fabs(it->get<double>()) <= ps::kAzimuthLimitDeg);
    };
    const auto oneOf = [](const json& o, const char* key, std::initializer_list<const char*> values) {
        const auto it = o.find(key);
        if (it == o.end()) {
            return true;
        }
        if (!it->is_string()) {
            return false;
        }
        for (const char* v : values) {
            if (it->get<std::string>() == v) {
                return true;
            }
        }
        return false;
    };
    const auto pick = [this](std::initializer_list<const char*> keys) {
        json out = json::object();
        for (const char* k : keys) {
            out[k] = m_op[k];
        }
        return out;
    };
    const bool searching = m_opState == "OP_SURVEY" || m_opState == "OP_ACQUIRE" || m_opState == "OP_TRACK" ||
                           m_opState == "OP_LOSS" || m_opState == "OP_LIMITED_SURVEY";
    // Таблица 4: в каких состояниях вложенного КА выполняется команда. Не принятая
    // в OP_INITIALIZATION и OP_CALIBRATION — BUSY, в остальных — NOT_SUPPORTED.
    bool accepted = true;
    if (t == ps::msg::SetOpFrequency) {
        accepted = m_opState == "OP_INITIALIZATION" || m_opState == "OP_IDLE";
    } else if (t == ps::msg::SetOpStartCalibration || t == ps::msg::SetOpStartSurvey) {
        accepted = m_opState == "OP_IDLE";
    } else if (t == ps::msg::SetOpStartLoss) {
        accepted = m_opState == "OP_TRACK";
    } else if (t == ps::msg::SetOpStartAcquire || t == ps::msg::SetOpStop || t == ps::msg::SetOpStartHold ||
               t == ps::msg::SetOpStopHold) {
        accepted = searching;
    }
    if (!accepted) {
        const bool busy = m_opState == "OP_INITIALIZATION" || m_opState == "OP_CALIBRATION";
        reply(cmd, busy ? ps::Status::Busy : ps::Status::NotSupported);
        return;
    }

    if (t == ps::msg::SetOpFrequency) {
        const auto f = in.find("freq");
        if (f == in.end() || !f->is_number_integer() || f->get<std::int64_t>() < ps::kSffFrequencyMinHz ||
            f->get<std::int64_t>() > ps::kSffFrequencyMaxHz) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_op["freq"] = f->get<std::int64_t>();
        // OP_INITIALIZATION → OP_CALIBRATION → калибровка → OP_IDLE, затем ответ.
        m_op["cal"] = "OK";
        m_opState = "OP_IDLE";
        reply(cmd, ps::Status::Ok, pick({"freq"}));
    } else if (t == ps::msg::GetOpFrequency) {
        reply(cmd, ps::Status::Ok, pick({"freq"}));
    } else if (t == ps::msg::SetOpPolicy) {
        if (!oneOf(in, "acq", {"AUTO", "CONFIRM"}) || !oneOf(in, "loss", {"AUTO", "CONFIRM"})) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        for (const char* k : {"acq", "loss"}) {
            if (in.contains(k)) {
                m_op[k] = in.at(k);
            }
        }
        reply(cmd, ps::Status::Ok, pick({"acq", "loss"}));
    } else if (t == ps::msg::GetOpPolicy) {
        reply(cmd, ps::Status::Ok, pick({"acq", "loss"}));
    } else if (t == ps::msg::SetOpRay) {
        if (!oneOf(in, "mode", {"SCAN", "FIXED"}) || !azOk(in, "az_min") || !azOk(in, "az_max") ||
            !azOk(in, "az_fix")) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        for (const char* k : {"mode", "az_min", "az_max", "az_fix"}) {
            if (in.contains(k)) {
                m_op[k] = in.at(k);
            }
        }
        reply(cmd, ps::Status::Ok, pick({"mode", "az_min", "az_max", "az_fix"}));
    } else if (t == ps::msg::GetOpRay) {
        reply(cmd, ps::Status::Ok, pick({"mode", "az_min", "az_max", "az_fix"}));
    } else if (t == ps::msg::SetOpData) {
        if (!in.contains("data") || !oneOf(in, "data", {"AUTO", "FPGA", "VIDEO"})) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_op["data"] = in.at("data");
        reply(cmd, ps::Status::Ok, pick({"data"}));
    } else if (t == ps::msg::GetOpData) {
        reply(cmd, ps::Status::Ok, pick({"data"}));
    } else if (t == ps::msg::SetOpStartCalibration) {
        m_op["cal"] = "OK";
        m_opState = "OP_IDLE";
        reply(cmd, ps::Status::Ok, pick({"cal"}));
    } else if (t == ps::msg::GetOpCalibration) {
        reply(cmd, ps::Status::Ok, pick({"cal"}));
    } else if (t == ps::msg::SetOpStartSurvey) {
        m_opState = "OP_SURVEY";
        reply(cmd, ps::Status::Ok);
        sendEvent(ps::msg::SendOpSurveyResult,
                  json{{"tgt", json::array({json{{"az", 12.0}, {"w", 10.0}, {"snr", 18.5}, {"cls", "INTERF"},
                                                 {"conf", 0.87}, {"coh", 64}, {"df", 48}},
                                            json{{"az", -7.5}, {"w", 20.0}, {"snr", 11.0}, {"cls", "DRONE"},
                                                 {"conf", 0.6}, {"coh", 40}, {"df", 33}}})}});
    } else if (t == ps::msg::SetOpStartAcquire) {
        if (!in.contains("az") || !azOk(in, "az")) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        const double az = in.at("az").get<double>();
        m_opState = "OP_TRACK";
        reply(cmd, ps::Status::Ok, json{{"az", az}});
        for (const char* st : {"ACQUIRE", "TRACK"}) {
            sendEvent(ps::msg::SendOpTrackResult,
                      json{{"st", st}, {"az_ray", az}, {"az", az + 0.4}, {"az_pred", az + 0.6}, {"az_err", 0.2},
                           {"az_rate", 0.5}, {"w", 10.0}, {"snr", 17.9}, {"cls", "INTERF"}, {"conf", 0.91},
                           {"coh", 70}, {"df", 55}, {"src", "FPGA"}, {"meth", "SD"}});
        }
    } else if (t == ps::msg::SetOpStartLoss) {
        m_opState = "OP_LIMITED_SURVEY";
        reply(cmd, ps::Status::Ok);
    } else if (t == ps::msg::SetOpStop) {
        m_opState = "OP_IDLE";
        reply(cmd, ps::Status::Ok);
    } else if (t == ps::msg::SetOpStartHold) {
        if (!in.contains("az") || !azOk(in, "az")) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        if (m_op["mode"] == "FIXED") {
            reply(cmd, ps::Status::NotSupported);
            return;
        }
        m_op["hold"] = true;
        reply(cmd, ps::Status::Ok, json{{"az", in.at("az")}});
    } else if (t == ps::msg::SetOpStopHold) {
        m_op["hold"] = false;
        reply(cmd, ps::Status::Ok);
    } else {
        reply(cmd, ps::Status::NotSupported);
    }
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
        const std::string state = m_mode == ps::Mode::Idle          ? (m_fault ? "FAULT" : "READY")
                                  : m_mode == ps::Mode::Operational ? m_opState
                                  : m_mode == ps::Mode::FullScanFreq ? m_sffState
                                                                     : "";
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
        // Таблица 1: SERVICE — только из IDLE; FULL_SCAN_FREQ и OPERATIONAL — из IDLE
        // или друг из друга. Статус отказа документ не задаёт — отвечаем INVALID_ARGUMENT.
        const bool fromService = m_mode == ps::Mode::Service && target != ps::Mode::Service && target != ps::Mode::Idle;
        const bool toService = target == ps::Mode::Service && m_mode != ps::Mode::Idle && m_mode != ps::Mode::Service;
        if (fromService || toService) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        // Из FULL_SCAN_FREQ — только в SFF_IDLE, из OPERATIONAL — в OP_INITIALIZATION
        // и OP_IDLE (рисунки 3, 4, таблица 4).
        if (target != m_mode && m_mode == ps::Mode::FullScanFreq && m_sffState != "SFF_IDLE") {
            reply(cmd, ps::Status::Busy);
            return;
        }
        if (target != m_mode && m_mode == ps::Mode::Operational && m_opState != "OP_INITIALIZATION" &&
            m_opState != "OP_IDLE") {
            reply(cmd, m_opState == "OP_CALIBRATION" ? ps::Status::Busy : ps::Status::NotSupported);
            return;
        }
        if (target == ps::Mode::FullScanFreq && m_mode != ps::Mode::FullScanFreq) {
            m_sffState = "SFF_IDLE";
        }
        if (m_mode == ps::Mode::Service && target != ps::Mode::Service) {
            resetService();
        }
        if (target == ps::Mode::Operational && m_mode != ps::Mode::Operational) {
            m_opState = "OP_INITIALIZATION";
        }
        m_mode = target;
        reply(cmd, ps::Status::Ok, json{{"mode", std::string(ps::toString(m_mode))}});
        sendTelemetry();
        return;
    }

    // OSD — в любом режиме (v8 п. 3.3.1).
    if (t == ps::msg::SetOsd || t == ps::msg::GetOsd) {
        if (t == ps::msg::SetOsd) {
            ps::OsdParams p;
            if (!ps::OsdParams::fromJson(in, p) || in.contains("los")) {
                reply(cmd, ps::Status::InvalidArgument);
                return;
            }
            m_osd.update(p.toJson());
        }
        json out = m_osd;
        out["los"] = 0;
        reply(cmd, ps::Status::Ok, out);
        return;
    }
    // Самопроверка и перезапуск — только в IDLE/READY (v8 п. 3.2).
    if (t == ps::msg::SetStartTest || t == ps::msg::SetReset) {
        if (m_mode != ps::Mode::Idle) {
            reply(cmd, ps::Status::NotSupported);
            return;
        }
        if (t == ps::msg::SetStartTest) {
            ++m_selfTests;
            reply(cmd, ps::Status::Ok);
            return;
        }
        ++m_resets;
        reply(cmd, ps::Status::Ok);
        // Ответ ушёл — перезапуск: TCP закрывается, после него снова нужен hello.
        m_helloDone = false;
        m_tlmTimer.stop();
        if (m_client) {
            m_client->flush();
            m_client->disconnectFromHost();
        }
        return;
    }

    if (t == ps::msg::SetSffParameters || t == ps::msg::GetSffParameters || t == ps::msg::SetSffStart ||
        t == ps::msg::SetSffStop) {
        handleSff(cmd);
        return;
    }

    if (t.rfind("set_op_", 0) == 0 || t.rfind("get_op_", 0) == 0) {
        handleOp(cmd);
        return;
    }

    const bool serviceCommand =
        t == ps::msg::SetPhaseAtten || t == ps::msg::GetPhaseAtten ||
        t == ps::msg::SetBeamAzimuth || t == ps::msg::SetBeam ||
        t == ps::msg::GetBeam || t == ps::msg::Rs485Send || t == ps::msg::SetLo ||
        t == ps::msg::GetLo || t == ps::msg::SetMixer || t == ps::msg::GetMixer ||
        t == ps::msg::SetVideo || t == ps::msg::GetVideo || t == ps::msg::GetRfmTelemetry ||
        t == ps::msg::GetTrxStatus || t == ps::msg::GetPlStatus || t == ps::msg::SetOsdText ||
        t == ps::msg::ClearOsdText;
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
    } else if (t == ps::msg::SetOsdText) {
        json p;
        const auto row = in.find("row");
        const auto col = in.find("col");
        const auto text = in.find("text");
        if (row == in.end() || !row->is_number_integer() || col == in.end() || !col->is_number_integer() ||
            text == in.end() || !text->is_string() ||
            !ps::osdTextPayload(row->get<int>(), col->get<int>(), text->get<std::string>(),
                                m_osd.value("std", "PAL") == "NTSC", p)) {
            reply(cmd, ps::Status::InvalidArgument);
            return;
        }
        m_osdText = p;
        reply(cmd, ps::Status::Ok, p);
    } else if (t == ps::msg::ClearOsdText) {
        m_osdText = json::object();
        reply(cmd, ps::Status::Ok);
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
        // В ответе set_* — установленные значения и телеметрия (v8 п. 3.6.1.5).
        reply(cmd, ps::Status::Ok, lo);
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
        reply(cmd, ps::Status::Ok, m_video);
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
        reply(cmd, ps::Status::Ok, json{{"ch", answer}, {"temp", m_mixer["temp"]}});
    }
}

void TestPsServer::sendTelemetry()
{
    if (!m_helloDone || m_tlmPort == 0 || m_tlmOff) {
        return;
    }
    // Всё исправно: все биты 1, резервные тоже (таблица 5).
    QByteArray packet = m_statusBytes.isEmpty() ? QByteArray(10, char(0xff)) : m_statusBytes;
    if (m_fault) {
        packet[9] = char(0x7f); // бит 79: самопроверка калибровочных таблиц
    }
    m_udp.writeDatagram(packet, QHostAddress(m_tlmHost), m_tlmPort);
}
