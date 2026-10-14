#include "UdpFloodGenerator.h"

#include "PsProtocol.h"

#include <QDateTime>

#include <algorithm>

namespace ps = afar::ps;
using nlohmann::json;

UdpFloodGenerator::UdpFloodGenerator(QObject* parent)
    : QObject(parent)
{
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, &UdpFloodGenerator::tick);
}

bool UdpFloodGenerator::start(quint16 port, int perSecond, qint64 durationMs)
{
    if (port == 0 || perSecond <= 0) {
        return false;
    }
    m_port = port;
    m_rate = perSecond;
    m_durationMs = durationMs;
    m_sent = 0;
    m_clock.start();
    m_timer.start();
    return true;
}

void UdpFloodGenerator::stop()
{
    if (!m_timer.isActive()) {
        return;
    }
    m_timer.stop();
    emit finished(m_sent);
}

void UdpFloodGenerator::tick()
{
    const qint64 elapsed = m_clock.elapsed();
    const bool last = m_durationMs > 0 && elapsed >= m_durationMs;
    const qint64 until = last ? m_durationMs : elapsed;
    // Сколько датаграмм должно было уйти к этому моменту; за такт не больше 2000, чтобы не вешать окно.
    const qint64 due = until * m_rate / 1000;
    qint64 budget = std::min<qint64>(2000, due - static_cast<qint64>(m_sent));
    while (budget-- > 0) {
        ps::Envelope e;
        e.msg_type = std::string(ps::msg::Telemetry);
        e.msg_class = ps::MsgClass::Event;
        e.msg_count = m_count++;
        e.payload = json{{"time", QDateTime::currentMSecsSinceEpoch()},
                         {"uptime", elapsed / 1000},
                         {"mode", "IDLE"},
                         {"state", "READY"},
                         {"devices", json{{"power", "OK"}, {"rs485", "OK"}}},
                         {"temperature", json{{"pl", 48.5}}},
                         {"power", json{{"v12", 12.0}}}};
        const std::string dg = ps::encodeDatagram(e);
        m_socket.writeDatagram(dg.data(), static_cast<qint64>(dg.size()), QHostAddress::LocalHost, m_port);
        ++m_sent;
    }
    if (last) {
        stop();
    }
}
