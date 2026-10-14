#include "PsLink.h"

#include <QHostAddress>
#include <QNetworkDatagram>

#include <algorithm>

namespace ps = afar::ps;

namespace {

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

}  // namespace

PsLink::PsLink(QObject* parent)
    : QObject(parent)
    , m_socket(this)
    , m_udp(this)
    , m_tick(this)
    , m_reconnect(this)
{
    qRegisterMetaType<ps::Envelope>();
    qRegisterMetaType<ps::Status>();
    qRegisterMetaType<ps::Telemetry>();
    m_clock.start();
    m_tick.setInterval(100);
    m_reconnect.setSingleShot(true);

    connect(&m_socket, &QTcpSocket::connected, this, &PsLink::onConnected);
    connect(&m_socket, &QTcpSocket::readyRead, this, &PsLink::onReadyRead);
    connect(&m_socket, &QTcpSocket::disconnected, this, &PsLink::onDisconnected);
    connect(&m_socket, &QAbstractSocket::errorOccurred, this, &PsLink::onSocketError);
    connect(&m_udp, &QUdpSocket::readyRead, this, &PsLink::onUdpReadyRead);
    connect(&m_tick, &QTimer::timeout, this, &PsLink::onTick);
    connect(&m_reconnect, &QTimer::timeout, this, &PsLink::startConnect);
}

PsLink::~PsLink()
{
    m_wanted = false;
    m_socket.abort();
}

void PsLink::connectToPs(const QString& host, quint16 port, quint16 telemetryPort)
{
    m_host = host.trimmed();
    m_port = port;
    if (telemetryPort != m_requestedTelemetryPort) {
        m_udp.close();
    }
    m_requestedTelemetryPort = telemetryPort;
    m_reconnect.stop();
    // Закрытие прежнего соединения — наше решение, а не обрыв: ошибки и автопереподключения не нужны.
    m_wanted = false;
    m_socket.abort();
    m_wanted = true;
    m_reachedReady = false;
    m_lastError.clear();
    startConnect();
}

void PsLink::disconnectFromPs()
{
    m_wanted = false;
    m_reconnect.stop();
    m_tick.stop();
    m_pending.clear();
    m_socket.abort();
    m_udp.close();
    m_splitter.clear();
    setState(State::Disconnected);
}

quint16 PsLink::telemetryPort() const
{
    return m_udp.state() == QAbstractSocket::BoundState ? m_udp.localPort() : 0;
}

int PsLink::sendCommand(std::string_view msgType, const nlohmann::json& payload)
{
    if (m_state != State::Ready) {
        return -1;
    }
    return queueCommand(msgType, payload);
}

int PsLink::queueCommand(std::string_view msgType, const nlohmann::json& payload)
{
    const quint8 count = ++m_txCount;
    Pending p;
    p.msgType = qs(msgType);
    p.msgCount = count;
    p.deadlineMs = m_clock.elapsed() + m_timeouts.commandMs;
    m_pending.insert(pendingKey(p.msgType, count), p);
    writeEnvelope(ps::makeCommand(msgType, count, payload));
    return count;
}

void PsLink::setState(State s)
{
    if (m_state == s) {
        return;
    }
    m_state = s;
    emit stateChanged(s);
}

void PsLink::startConnect()
{
    if (!m_wanted || m_host.isEmpty()) {
        return;
    }
    m_splitter.clear();
    m_pending.clear();
    m_peerVersion.clear();
    if (!bindTelemetry()) {
        // Без телеметрии канал управления всё равно нужен: сообщаем и продолжаем.
        m_lastError = QStringLiteral("порт телеметрии UDP %1 занят: %2")
                          .arg(m_requestedTelemetryPort)
                          .arg(m_udp.errorString());
        emit errorOccurred(m_lastError);
    }
    setState(State::Connecting);
    m_connectStartMs = m_clock.elapsed();
    m_tick.start();
    m_socket.connectToHost(m_host, m_port);
}

bool PsLink::bindTelemetry()
{
    if (m_udp.state() == QAbstractSocket::BoundState) {
        return true;
    }
    m_udp.close();
    m_lastTlmCount = -1;
    return m_udp.bind(QHostAddress::AnyIPv4, m_requestedTelemetryPort,
                      QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint);
}

void PsLink::dropLink(const QString& reason)
{
    m_lastError = reason;
    m_pending.clear();
    m_socket.abort();
    m_splitter.clear();
    // Таймеры настраиваем до сигналов: подписчик в ответ на разрыв может сразу начать
    // новое подключение (поиск платы по MAC находит адрес в таблице ARP мгновенно),
    // и мы не должны остановить таймеры этой новой попытки.
    if (m_wanted && m_autoReconnect && m_reachedReady) {
        m_reconnect.start(m_timeouts.reconnectMs);
    } else {
        m_tick.stop();
    }
    emit errorOccurred(reason);
    setState(State::Disconnected);
}

void PsLink::writeEnvelope(const ps::Envelope& env)
{
    const std::string line = ps::encodeLine(env);
    m_socket.write(line.data(), static_cast<qint64>(line.size()));
    m_lastTxMs = m_clock.elapsed();
    emit frameLogged(true, QStringLiteral("TCP"), qs(line).trimmed());
}

void PsLink::onConnected()
{
    m_lastRxMs = m_clock.elapsed();
    setState(State::Handshaking);
    // Телеметрию PS шлёт на тот адрес, с которого пришло соединение.
    QHostAddress local = m_socket.localAddress();
    bool isV4 = false;
    const quint32 v4 = local.toIPv4Address(&isV4);
    if (isV4) {
        local = QHostAddress(v4);
    }
    ps::Hello hello;
    hello.tlm_ip = local.toString().toStdString();
    hello.tlm_port = telemetryPort() ? telemetryPort() : m_requestedTelemetryPort;
    queueCommand(ps::msg::Hello, hello.toJson());
}

void PsLink::onReadyRead()
{
    const QByteArray bytes = m_socket.readAll();
    if (!m_splitter.append(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())))) {
        dropLink(QStringLiteral("PS прислал строку длиннее 1 МиБ"));
        return;
    }
    std::string line;
    while (m_splitter.takeLine(line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        handleLine(line);
        if (m_state == State::Disconnected) {
            return;
        }
    }
}

void PsLink::handleLine(const std::string& line)
{
    m_lastRxMs = m_clock.elapsed();
    emit frameLogged(false, QStringLiteral("TCP"), qs(line));
    ps::Envelope env;
    std::string error;
    if (!ps::decodeLine(line, env, &error)) {
        // Битую строку пропускаем, соединение не рвём.
        qWarning("PsLink: строка от %s отброшена: %s", qPrintable(m_host), error.c_str());
        return;
    }
    switch (env.msg_class) {
    case ps::MsgClass::Response:
        handleResponse(env);
        break;
    case ps::MsgClass::Event:
        if (m_state == State::Ready) {
            emit eventReceived(env);
        }
        break;
    case ps::MsgClass::Command:
        qWarning("PsLink: PS прислал команду %s, игнорируем", env.msg_type.c_str());
        break;
    }
}

void PsLink::handleResponse(const ps::Envelope& env)
{
    const QString type = qs(env.msg_type);
    const QString key = pendingKey(type, env.msg_count);
    if (!m_pending.contains(key)) {
        qWarning("PsLink: ответ %s #%d без запроса", env.msg_type.c_str(), env.msg_count);
        return;
    }
    m_pending.remove(key);
    ps::Status status = ps::Status::InvalidArgument;
    if (!ps::responseStatus(env, status)) {
        qWarning("PsLink: в ответе %s нет корректного status", env.msg_type.c_str());
        emit errorOccurred(QStringLiteral("ответ %1 без корректного status").arg(type));
        return;
    }
    if (env.msg_type == ps::msg::Hello) {
        const auto ver = env.payload.find("ver");
        m_peerVersion = ver != env.payload.end() && ver->is_string()
                            ? QString::fromStdString(ver->get<std::string>())
                            : QString();
        if (status != ps::Status::Ok) {
            m_wanted = false;
            dropLink(QStringLiteral("PS отклонил hello: %1 (версия PS %2)")
                         .arg(QString::fromStdString(ps::statusRu(status)),
                              m_peerVersion.isEmpty() ? QStringLiteral("—") : m_peerVersion));
            return;
        }
        if (!ps::versionsCompatible(m_peerVersion.toStdString(), ps::kProtocolVersion)) {
            m_wanted = false;
            dropLink(QStringLiteral("несовместимая версия протокола PS: %1").arg(m_peerVersion));
            return;
        }
        m_reachedReady = true;
        setState(State::Ready);
    }
    if (env.msg_type == ps::msg::KeepAlive) {
        return;
    }
    emit responseReceived(env, status);
}

void PsLink::onSocketError()
{
    if (!m_wanted || m_state == State::Disconnected) {
        return;
    }
    dropLink(QStringLiteral("%1:%2 — %3").arg(m_host).arg(m_port).arg(m_socket.errorString()));
}

void PsLink::onDisconnected()
{
    if (!m_wanted || m_state == State::Disconnected) {
        return;
    }
    dropLink(QStringLiteral("PS закрыл соединение"));
}

void PsLink::onUdpReadyRead()
{
    // Защита от потока датаграмм: за один проход разбираем не больше kUdpPerPass штук, остальное —
    // следующим проходом цикла событий, чтобы окно успевало перерисовываться и отвечать на ввод.
    int processed = 0;
    while (m_udp.hasPendingDatagrams()) {
        if (processed >= kUdpPerPass) {
            QTimer::singleShot(0, this, &PsLink::onUdpReadyRead);
            return;
        }
        ++processed;
        const QNetworkDatagram dg = m_udp.receiveDatagram(static_cast<qint64>(ps::kMaxDatagramBytes) + 1);
        const QByteArray data = dg.data();
        ps::Envelope env;
        ps::Telemetry tlm;
        if (data.size() > static_cast<qsizetype>(ps::kMaxDatagramBytes) ||
            !ps::decodeLine(std::string_view(data.constData(), static_cast<std::size_t>(data.size())), env) ||
            env.msg_type != ps::msg::Telemetry || !ps::Telemetry::fromJson(env.payload, tlm)) {
            ++m_tlmRejected;
            emit frameLogged(false, QStringLiteral("UDP"), QString::fromUtf8(data.left(512)));
            continue;
        }
        if (m_lastTlmCount >= 0) {
            const int expected = (m_lastTlmCount + 1) & 0xFF;
            const int gap = (int(env.msg_count) - expected + 256) & 0xFF;
            // Большой разрыв — скорее перезапуск PS, а не потери.
            if (gap > 0 && gap < 128) {
                m_tlmLost += quint64(gap);
            }
        }
        m_lastTlmCount = env.msg_count;
        ++m_tlmReceived;
        emit frameLogged(false, QStringLiteral("UDP"), QString::fromUtf8(data));
        // Подписчикам (экран, спектр) телеметрию отдаём не чаще kTlmEmitMinMs: счётчики при этом точные.
        const qint64 now = m_clock.elapsed();
        if (now - m_lastTlmEmitMs >= kTlmEmitMinMs) {
            m_lastTlmEmitMs = now;
            emit telemetryReceived(tlm);
        }
    }
}

void PsLink::onTick()
{
    const qint64 now = m_clock.elapsed();
    if (m_state == State::Connecting) {
        if (now - m_connectStartMs > m_timeouts.connectMs) {
            dropLink(QStringLiteral("%1:%2 не отвечает").arg(m_host).arg(m_port));
        }
        return;
    }
    if (m_state != State::Handshaking && m_state != State::Ready) {
        return;
    }

    QVector<Pending> expired;
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now > it->deadlineMs) {
            expired.append(*it);
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
    for (const Pending& p : expired) {
        if (p.msgType == qs(ps::msg::Hello)) {
            dropLink(QStringLiteral("PS не ответил на hello"));
            return;
        }
        if (p.msgType == qs(ps::msg::KeepAlive)) {
            dropLink(QStringLiteral("PS не ответил на keep_alive"));
            return;
        }
        emit commandTimedOut(p.msgType, p.msgCount);
    }

    const qint64 silence = qint64(m_timeouts.keepAliveIdleMs) + 3000;
    if (now - m_lastRxMs > silence) {
        dropLink(QStringLiteral("PS молчит дольше %1 с").arg(silence / 1000));
        return;
    }
    if (m_state == State::Ready && now - std::max(m_lastRxMs, m_lastTxMs) >= m_timeouts.keepAliveIdleMs &&
        !keepAlivePending()) {
        sendCommand(ps::msg::KeepAlive);
    }
}

bool PsLink::keepAlivePending() const
{
    const QString ka = qs(ps::msg::KeepAlive);
    return std::any_of(m_pending.cbegin(), m_pending.cend(),
                       [&ka](const Pending& p) { return p.msgType == ka; });
}

QString PsLink::pendingKey(const QString& msgType, int count)
{
    return msgType + QLatin1Char('#') + QString::number(count);
}
