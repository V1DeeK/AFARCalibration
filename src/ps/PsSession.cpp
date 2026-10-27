#include "PsSession.h"

#include "MacResolver.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QSettings>
#include <QTimer>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

const QString kGroup = QStringLiteral("ps/");

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

quint16 savedPort(const QString& key, quint16 fallback)
{
    const int p = QSettings().value(kGroup + key, fallback).toInt();
    return (p > 0 && p <= 65535) ? static_cast<quint16>(p) : fallback;
}

bool isIpLiteral(const QString& host)
{
    return !QHostAddress(host).isNull();
}

}  // namespace

PsSession::PsSession(QObject* parent)
    : QObject(parent)
    , m_link(new PsLink(this))
    , m_resolver(new MacResolver(this))
    , m_retry(new QTimer(this))
{
    qRegisterMetaType<PsSession::LogLine>();
    {
        PsLink::Timeouts t = m_link->timeouts();
        t.keepAliveIdleMs = keepAliveMs();
        m_link->setTimeouts(t);
    }
    connect(m_link, &PsLink::stateChanged, this, &PsSession::onLinkState);
    connect(m_link, &PsLink::responseReceived, this, &PsSession::onResponse);
    connect(m_link, &PsLink::eventReceived, this, &PsSession::onEvent);
    connect(m_link, &PsLink::telemetryReceived, this, &PsSession::onTelemetry);
    connect(m_link, &PsLink::frameLogged, this, &PsSession::onFrame);
    connect(m_link, &PsLink::errorOccurred, this,
            [this](const QString& text) { emit message(QStringLiteral("Ошибка связи: %1").arg(text)); });
    connect(m_link, &PsLink::commandTimedOut, this, [this](const QString& type, int count) {
        emit message(QStringLiteral("Нет ответа PS на %1 №%2").arg(type).arg(count));
    });

    m_retry->setSingleShot(true);
    m_retry->setInterval(kSearchRetryMs);
    connect(m_retry, &QTimer::timeout, this, [this] {
        if (!autoConnect() || m_seriesActive || m_link->isReady() || searching()) {
            return;
        }
        m_macSearchDone = false;
        findBoard();
    });

    connect(m_resolver, &MacResolver::portOpenHosts, this,
            [this](const QVector<QPair<QString, QString>>& hosts) {
                if (hosts.size() == 1) {
                    emit boardCandidate(hosts.first().first, hosts.first().second);
                }
            });
    connect(m_resolver, &MacResolver::progress, this, [this](const QString& text) {
        m_searchText = text;
        emit statusChanged();
    });
    connect(m_resolver, &MacResolver::failed, this, [this](const QString& reason) {
        m_searchText.clear();
        m_lastError = reason;
        emit message(reason);
        scheduleRetry();
        emit statusChanged();
    });
    connect(m_resolver, &MacResolver::resolved, this, [this](const QString& ip, const QString& mac) {
        m_searchText.clear();
        emit message(QStringLiteral("Плата %1 найдена по MAC: %2").arg(mac, ip));
        connectToFound(ip);
    });
}

void PsSession::connectToFound(const QString& ip)
{
    // Имя, введённое оператором в поле адреса, найденный IP не затирает.
    const QString saved = host();
    if (saved.isEmpty() || isIpLiteral(saved)) {
        QSettings().setValue(kGroup + QStringLiteral("host"), ip);
        emit hostResolved(ip);
    }
    // Оператор успел нажать «Отключиться» — найденный адрес только запоминаем.
    if (!autoConnect() || m_seriesActive || (m_link->isReady() && m_link->host() == ip)) {
        emit statusChanged();
        return;
    }
    const bool searched = m_macSearchDone;
    m_link->connectToPs(ip, port(), telemetryPort());
    m_macSearchDone = searched;
    emit statusChanged();
}

PsSession::~PsSession() = default;

QString PsSession::host() const
{
    return QSettings().value(kGroup + QStringLiteral("host")).toString().trimmed();
}

quint16 PsSession::port() const
{
    return savedPort(QStringLiteral("port"), ps::kDefaultControlPort);
}

quint16 PsSession::telemetryPort() const
{
    return savedPort(QStringLiteral("tlm_port"), ps::kDefaultTelemetryPort);
}

QString PsSession::mac() const
{
    QSettings s;
    const QString key = kGroup + QStringLiteral("mac");
    if (s.contains(key)) {
        // Оператор мог оставить поле пустым: тогда плату по MAC не ищем.
        const QString saved = s.value(key).toString();
        return MacResolver::isValidMac(saved) ? MacResolver::formatMac(saved) : QString();
    }
    const QString legacy = s.value(QStringLiteral("board/mac")).toString();
    if (MacResolver::isValidMac(legacy)) {
        return MacResolver::formatMac(legacy);
    }
    return QString::fromLatin1(kDefaultMac);
}

QString PsSession::hostName() const
{
    QSettings s;
    const QString key = kGroup + QStringLiteral("hostname");
    // Пустое поле — оператор решил по имени не искать.
    return s.contains(key) ? s.value(key).toString().trimmed() : QString::fromLatin1(kDefaultHostName);
}

void PsSession::setHostName(const QString& name)
{
    QSettings().setValue(kGroup + QStringLiteral("hostname"), name.trimmed());
}

int PsSession::keepAliveMs() const
{
    const int ms = QSettings().value(kGroup + QStringLiteral("keepalive_ms"), kKeepAliveDefaultMs).toInt();
    return qBound(kKeepAliveMinMs, ms, kKeepAliveMaxMs);
}

void PsSession::setKeepAliveMs(int ms)
{
    ms = qBound(kKeepAliveMinMs, ms, kKeepAliveMaxMs);
    QSettings().setValue(kGroup + QStringLiteral("keepalive_ms"), ms);
    PsLink::Timeouts t = m_link->timeouts();
    t.keepAliveIdleMs = ms;
    m_link->setTimeouts(t);
}

bool PsSession::autoConnect() const
{
    return QSettings().value(kGroup + QStringLiteral("auto_connect"), false).toBool();
}

void PsSession::saveSettings(const QString& host, quint16 port, quint16 telemetryPort, const QString& mac)
{
    QSettings s;
    if (!host.isEmpty()) {
        s.setValue(kGroup + QStringLiteral("host"), host);
    }
    s.setValue(kGroup + QStringLiteral("port"), port);
    s.setValue(kGroup + QStringLiteral("tlm_port"), telemetryPort);
    if (MacResolver::isValidMac(mac)) {
        // Тот же MAC читает «Параметры платы» (libiio): плата одна.
        s.setValue(kGroup + QStringLiteral("mac"), MacResolver::formatMac(mac));
        s.setValue(QStringLiteral("board/mac"), MacResolver::formatMac(mac));
    } else if (mac.trimmed().isEmpty()) {
        s.setValue(kGroup + QStringLiteral("mac"), QString());
    }
    s.setValue(kGroup + QStringLiteral("auto_connect"), true);
}

void PsSession::refuse(const QString& text)
{
    m_lastError = text;
    emit message(text);
    emit statusChanged();
}

bool PsSession::connectTo(const QString& host, quint16 port, quint16 telemetryPort, const QString& mac)
{
    if (m_seriesActive) {
        refuse(QStringLiteral("Идёт серия: PS занят оркестратором"));
        return false;
    }
    const QString h = host.trimmed();
    const QString m = mac.trimmed();
    if (!m.isEmpty() && !MacResolver::isValidMac(m)) {
        refuse(QStringLiteral("MAC-адрес указан неверно: %1. Формат AA:BB:CC:DD:EE:FF").arg(m));
        return false;
    }
    if (h.isEmpty()) {
        if (hostName().isEmpty()) {
            if (m.isEmpty()) {
                refuse(QStringLiteral("Укажите IP-адрес, имя в сети или MAC-адрес платы"));
                return false;
            }
            return findByMac(m, port, telemetryPort);
        }
        m_lastError.clear();
        saveSettings(QString(), port, telemetryPort, m);
        stopSearch();
        m_link->disconnectFromPs();
        m_macSearchDone = true;
        lookupName(hostName());
        return true;
    }
    m_lastError.clear();
    saveSettings(h, port, telemetryPort, m);
    startLink(h, port, telemetryPort);
    return true;
}

bool PsSession::findByMac(const QString& mac, quint16 port, quint16 telemetryPort)
{
    if (m_seriesActive) {
        refuse(QStringLiteral("Идёт серия: PS занят оркестратором"));
        return false;
    }
    if (!MacResolver::isValidMac(mac)) {
        refuse(QStringLiteral("MAC-адрес указан неверно: %1. Формат AA:BB:CC:DD:EE:FF").arg(mac));
        return false;
    }
    m_lastError.clear();
    saveSettings(QString(), port, telemetryPort, mac);
    m_link->disconnectFromPs();
    m_macSearchDone = true;
    stopSearch();
    m_resolver->resolve(mac, port);
    emit statusChanged();
    return true;
}

void PsSession::startLink(const QString& host, quint16 port, quint16 telemetryPort)
{
    stopSearch();
    if (!isIpLiteral(host)) {
        // Имя вместо IP: сначала узнаём адрес сами. Подключение по имени могло бы
        // уйти по IPv6, а hello передаёт PS адрес для телеметрии — нужен IPv4.
        m_link->disconnectFromPs();
        m_macSearchDone = true;
        lookupName(host);
        return;
    }
    m_macSearchDone = false;
    m_link->connectToPs(host, port, telemetryPort);
}

void PsSession::stopSearch()
{
    m_resolver->cancel();
    if (m_nameLookupId >= 0) {
        QHostInfo::abortHostLookup(m_nameLookupId);
        m_nameLookupId = -1;
    }
    m_retry->stop();
    m_searchText.clear();
}

void PsSession::lookupName(const QString& name)
{
    if (m_nameLookupId >= 0) {
        QHostInfo::abortHostLookup(m_nameLookupId);
    }
    m_lookupName = name;
    m_searchText = QStringLiteral("поиск платы по имени %1…").arg(name);
    m_nameLookupId = QHostInfo::lookupHost(name, this, &PsSession::onNameLookup);
    emit statusChanged();
}

void PsSession::onNameLookup(const QHostInfo& info)
{
    if (info.lookupId() != m_nameLookupId) {
        return;
    }
    m_nameLookupId = -1;
    m_searchText.clear();
    QString ip;
    for (const QHostAddress& a : info.addresses()) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol) {
            ip = a.toString();
            break;
        }
    }
    if (ip.isEmpty()) {
        emit message(QStringLiteral("Имя %1 в сети не найдено").arg(m_lookupName));
        startMacSearch();
        emit statusChanged();
        return;
    }
    if (ip == m_link->host() && !m_link->isReady() && !m_link->lastError().isEmpty()) {
        // Имя указывает на тот же адрес, что не отвечает: плата ещё загружается.
        emit message(QStringLiteral("%1 = %2, плата пока не отвечает — пробую снова").arg(m_lookupName, ip));
        scheduleRetry();
        emit statusChanged();
        return;
    }
    emit message(QStringLiteral("Плата найдена по имени %1: %2").arg(m_lookupName, ip));
    connectToFound(ip);
}

void PsSession::startMacSearch()
{
    const QString m = mac();
    if (!MacResolver::isValidMac(m)) {
        m_lastError = QStringLiteral("Плата по имени %1 не найдена, MAC не задан").arg(m_lookupName);
        scheduleRetry();
        return;
    }
    emit message(QStringLiteral("Ищу плату по MAC %1").arg(m));
    m_resolver->resolve(m, port());
}

void PsSession::setSearchRetryMs(int ms)
{
    m_retry->setInterval(ms);
}

void PsSession::scheduleRetry()
{
    if (!autoConnect() || m_seriesActive || m_link->isReady() || m_retry->isActive()) {
        return;
    }
    m_retry->start();
}

void PsSession::disconnectFromPs()
{
    QSettings().setValue(kGroup + QStringLiteral("auto_connect"), false);
    m_reconnectAfterSeries = false;
    stopSearch();
    m_link->disconnectFromPs();
    emit statusChanged();
}

void PsSession::connectFromSettings()
{
    if (!autoConnect() || m_seriesActive) {
        return;
    }
    const QString h = host();
    if (!h.isEmpty()) {
        startLink(h, port(), telemetryPort());
    } else if (!hostName().isEmpty() || MacResolver::isValidMac(mac())) {
        connectTo(QString(), port(), telemetryPort(), mac());
    }
}

bool PsSession::searching() const
{
    return m_resolver->busy() || m_nameLookupId >= 0;
}

bool PsSession::busy() const
{
    return searching() || m_link->isTrying();
}

// Адрес не ответил — IP у платы мог смениться: ищем её по имени в сети, затем
// по MAC. PsLink тем временем сам стучится на старый адрес.
void PsSession::findBoard()
{
    if (m_macSearchDone || searching() || !autoConnect() || m_seriesActive) {
        return;
    }
    QString name = hostName();
    if (name.isEmpty() && !host().isEmpty() && !isIpLiteral(host())) {
        name = host();
    }
    const QString m = mac();
    if (name.isEmpty() && !MacResolver::isValidMac(m)) {
        return;
    }
    m_macSearchDone = true;
    const QString who = m_link->host().isEmpty() ? QStringLiteral("Плата") : m_link->host();
    if (!name.isEmpty()) {
        emit message(QStringLiteral("%1 не отвечает, ищу плату по имени %2").arg(who, name));
        lookupName(name);
        return;
    }
    emit message(QStringLiteral("%1 не отвечает, ищу плату по MAC %2").arg(who, m));
    m_resolver->resolve(m, port());
}

QString PsSession::statusText() const
{
    if (m_seriesActive) {
        return QStringLiteral("идёт серия: PS управляет оркестратор");
    }
    if (searching()) {
        return m_searchText.isEmpty() ? QStringLiteral("поиск платы…") : m_searchText;
    }
    switch (m_link->state()) {
    case PsLink::State::Disconnected: {
        const QString why = m_lastError.isEmpty() ? m_link->lastError() : m_lastError;
        if (why.isEmpty()) {
            return QStringLiteral("нет связи");
        }
        return m_link->isTrying()
                   ? QStringLiteral("нет связи: %1 — пробую снова").arg(why)
                   : QStringLiteral("нет связи: %1").arg(why);
    }
    case PsLink::State::Connecting:
        return QStringLiteral("подключение к %1:%2…").arg(m_link->host()).arg(m_link->port());
    case PsLink::State::Handshaking:
        return QStringLiteral("рукопожатие hello…");
    case PsLink::State::Ready:
        return QStringLiteral("на связи %1:%2, протокол %3, телеметрия UDP %4")
            .arg(m_link->host())
            .arg(m_link->port())
            .arg(m_link->peerVersion())
            .arg(m_link->telemetryPort());
    }
    return {};
}

void PsSession::onLinkState(PsLink::State state)
{
    switch (state) {
    case PsLink::State::Disconnected:
        m_modeKnown = false;
        m_modeAfterIdle.reset();
        m_modeAfterState.reset();
        m_state.clear();
        emit modeChanged();
        if (!m_link->lastError().isEmpty()) {
            if (m_macSearchDone) {
                scheduleRetry();
            } else {
                findBoard();
            }
        }
        break;
    case PsLink::State::Ready:
        m_macSearchDone = false;
        m_retry->stop();
        m_lastError.clear();
        m_link->sendCommand(ps::msg::GetState);
        // Адрес платы уже в таблице ARP: берём её настоящий MAC, чтобы поиск по MAC не искал чужой.
        QTimer::singleShot(300, this, [this] {
            if (!m_link->isReady()) {
                return;
            }
            const QString real = m_resolver->macForIp(m_link->host());
            if (real.isEmpty() || MacResolver::normalizeMac(real) == MacResolver::normalizeMac(mac())) {
                return;
            }
            QSettings s;
            s.setValue(kGroup + QStringLiteral("mac"), real);
            s.setValue(QStringLiteral("board/mac"), real);
            emit boardMacDetected(real);
        });
        break;
    case PsLink::State::Connecting:
    case PsLink::State::Handshaking:
        break;
    }
    emit statusChanged();
    emit controlsChanged();
}

void PsSession::setSeriesActive(bool active)
{
    if (active == m_seriesActive) {
        return;
    }
    m_seriesActive = active;
    if (active) {
        stopSearch();
        if (m_link->state() != PsLink::State::Disconnected) {
            // PS держит одно соединение: на время серии его занимает контроллер.
            m_reconnectAfterSeries = true;
            m_link->disconnectFromPs();
            emit message(QStringLiteral("Серия: связь вкладок с PS закрыта, PS управляет оркестратор"));
        }
    } else if (m_reconnectAfterSeries) {
        m_reconnectAfterSeries = false;
        startLink(m_link->host(), m_link->port(), telemetryPort());
    }
    emit statusChanged();
    emit controlsChanged();
}

void PsSession::setCheckRunning(bool running)
{
    if (running == m_checkRunning) {
        return;
    }
    m_checkRunning = running;
    emit controlsChanged();
}

bool PsSession::manualAllowed() const
{
    return m_link->isReady() && !m_seriesActive && !m_checkRunning;
}

int PsSession::send(std::string_view msgType, const json& payload)
{
    if (m_seriesActive) {
        refuse(QStringLiteral("Идёт серия: ручные команды PS заблокированы"));
        return -1;
    }
    if (msgType == ps::msg::SetSffStart) {
        clearSffResults();
    }
    const int count = m_link->sendCommand(msgType, payload);
    if (count < 0) {
        m_lastError = QStringLiteral("Нет связи с PS: %1 не отправлена").arg(qs(msgType));
        emit message(m_lastError);
    }
    return count;
}

void PsSession::setMode(ps::Mode mode, const QString& state)
{
    const bool changed = !m_modeKnown || mode != m_mode || (!state.isEmpty() && state != m_state);
    m_modeKnown = true;
    m_mode = mode;
    if (!state.isEmpty()) {
        m_state = state;
    }
    if (changed) {
        emit modeChanged();
    }
}

bool PsSession::canLeaveMode(QString* why) const
{
    if (!m_modeKnown || m_state.isEmpty()) {
        return true;
    }
    const QString st = m_state.toUpper();
    QString reason;
    switch (m_mode) {
    case ps::Mode::Idle:
        if (st != QStringLiteral("READY")) {
            reason = QStringLiteral("изделие в %1 — режим меняется только из READY")
                         .arg(QString::fromStdString(ps::stateRu(st.toStdString())));
        }
        break;
    case ps::Mode::FullScanFreq:
        if (st != QStringLiteral("SFF_IDLE")) {
            reason = QStringLiteral("идёт полное сканирование — сначала «Стоп»");
        }
        break;
    case ps::Mode::Operational:
        if (st == QStringLiteral("OP_CALIBRATION")) {
            reason = QStringLiteral("идёт калибровка каналов — дождитесь её конца");
        } else if (st != QStringLiteral("OP_INITIALIZATION") && st != QStringLiteral("OP_IDLE")) {
            reason = QStringLiteral("идёт поиск или сопровождение — сначала «Стоп» оперативного режима");
        }
        break;
    case ps::Mode::Service:
        break;
    }
    if (why) {
        *why = reason;
    }
    return reason.isEmpty();
}

int PsSession::requestMode(ps::Mode target)
{
    return requestModeNow(target, false);
}

int PsSession::requestModeNow(ps::Mode target, bool stateFresh)
{
    m_modeAfterIdle.reset();
    m_modeAfterState.reset();
    QString why;
    // Состояние вложенного КА в OPERATIONAL и FULL_SCAN_FREQ меняется без нас (поиск запущен,
    // обзор закончился, цель потеряна), а телеметрия его не несёт, поэтому перед уходом из
    // этих режимов состояние всегда уточняется get_state.
    const bool hasSubstate = m_mode == ps::Mode::Operational || m_mode == ps::Mode::FullScanFreq;
    const bool leaving = m_modeKnown && target != m_mode;
    const bool allowed = canLeaveMode(&why);
    if (!stateFresh && (!allowed || (leaving && hasSubstate))) {
        const int count = send(ps::msg::GetState);
        if (count >= 0) {
            m_modeAfterState = target;
            m_modeAfterStateCount = count;
        }
        return count;
    }
    if (!allowed) {
        refuse(QStringLiteral("Режим не сменён: %1").arg(why));
        emit modeRequestRefused(m_lastError);
        return -1;
    }
    const auto modeJson = [](ps::Mode m) { return json{{"mode", std::string(ps::toString(m))}}; };
    const bool direct = !m_modeKnown || m_mode == ps::Mode::Idle || m_mode == target || target == ps::Mode::Idle ||
                        (target != ps::Mode::Service && m_mode != ps::Mode::Service);
    if (direct) {
        return send(ps::msg::SetMode, modeJson(target));
    }
    const int count = send(ps::msg::SetMode, modeJson(ps::Mode::Idle));
    if (count >= 0) {
        m_modeAfterIdle = target;
    }
    return count;
}

void PsSession::onResponse(const ps::Envelope& r, ps::Status status)
{
    if (r.msg_type == ps::msg::SetMode && status != ps::Status::Ok) {
        m_modeAfterIdle.reset();
        emit modeRequestRefused(QStringLiteral("Режим не сменён: PS ответил %1 (%2)")
                                    .arg(qs(ps::toString(status)), QString::fromStdString(ps::statusRu(status))));
    }
    const bool ownStateReply = r.msg_type == ps::msg::GetState && m_modeAfterState
                               && static_cast<int>(r.msg_count) == m_modeAfterStateCount;
    if (ownStateReply && status != ps::Status::Ok) {
        m_modeAfterState.reset();
    }
    if (status != ps::Status::Ok) {
        emit message(QStringLiteral("%1: %2 (%3)")
                         .arg(qs(r.msg_type), QString::fromStdString(ps::statusRu(status)),
                              qs(ps::toString(status))));
        return;
    }
    std::string error;
    if (r.msg_type == ps::msg::GetState) {
        ps::StateInfo info;
        if (ps::StateInfo::fromJson(r.payload, info, &error)) {
            setMode(info.mode, QString::fromStdString(info.state));
        }
        if (ownStateReply) {
            const ps::Mode target = *m_modeAfterState;
            m_modeAfterState.reset();
            requestModeNow(target, true);
        }
    } else if (r.msg_type == ps::msg::SetMode) {
        ps::Mode mode{};
        const auto it = r.payload.find("mode");
        if (it != r.payload.end() && it->is_string() && ps::parseMode(it->get<std::string>(), mode)) {
            m_state.clear();
            setMode(mode, QString());
            // Переход через IDLE (таблица 1): IDLE достигнут — теперь в целевой режим.
            if (mode == ps::Mode::Idle && m_modeAfterIdle) {
                const ps::Mode target = *m_modeAfterIdle;
                m_modeAfterIdle.reset();
                send(ps::msg::SetMode, json{{"mode", std::string(ps::toString(target))}});
            }
        }
        m_link->sendCommand(ps::msg::GetState);
    } else if (r.msg_type == ps::msg::SetSffStart || r.msg_type == ps::msg::SetSffStop) {
        // SFF_IDLE ↔ SFF_FREQ: от состояния зависит, можно ли менять режим.
        m_link->sendCommand(ps::msg::GetState);
    } else if (r.msg_type == ps::msg::SetSffParameters || r.msg_type == ps::msg::GetSffParameters) {
        ps::SffParams p;
        if (ps::SffParams::fromJson(r.payload, p, &error)) {
            m_sffParams = p;
            emit sffChanged();
        }
    }
    if (!error.empty()) {
        emit message(QStringLiteral("%1: ответ не разобран — %2").arg(qs(r.msg_type), QString::fromStdString(error)));
    }
}

void PsSession::onEvent(const ps::Envelope& event)
{
    if (event.msg_type != ps::msg::SendSffResult) {
        return;
    }
    ps::SffResult result;
    std::string error;
    if (!ps::SffResult::fromJson(event.payload, result, &error)) {
        emit message(QStringLiteral("send_sff_result не разобран: %1").arg(QString::fromStdString(error)));
        return;
    }
    m_sffResults[result.freq] = std::move(result);
    emit sffChanged();
}

void PsSession::onTelemetry(const ps::Telemetry& t)
{
    m_telemetry = t;
    if (t.hasMode) {
        setMode(t.mode, QString::fromStdString(t.state));
    }
    emit telemetryChanged();
}

void PsSession::onFrame(bool outgoing, const QString& channel, const QString& line)
{
    // Защита журнала от лавины кадров: за окно в одну секунду пишем не больше kTcpLogPerSec строк TCP
    // и kUdpLogPerSec строк UDP, остальное считаем и сообщаем одной строкой «пропущено K».
    const qint64 now = m_logClock.isValid() ? m_logClock.elapsed() : 0;
    if (!m_logClock.isValid()) {
        m_logClock.start();
    }
    if (now - m_logWindowStartMs >= 1000) {
        if (m_logSkipped > 0) {
            appendLog(LogLine{QDateTime::currentDateTime(), false, QStringLiteral("СИС"),
                              QStringLiteral("Защита от перегрузки: в журнал не вошло %1 кадров за секунду "
                                             "(счётчики обмена и проверка потока их учитывают).")
                                  .arg(m_logSkipped)});
        }
        m_logWindowStartMs = now;
        m_logSkipped = 0;
        m_logTcpInWindow = 0;
        m_logUdpInWindow = 0;
    }
    int& used = channel == QLatin1String("UDP") ? m_logUdpInWindow : m_logTcpInWindow;
    const int limit = channel == QLatin1String("UDP") ? kUdpLogPerSec : kTcpLogPerSec;
    if (used >= limit) {
        ++m_logSkipped;
        return;
    }
    ++used;
    appendLog(LogLine{QDateTime::currentDateTime(), outgoing, channel, line.trimmed()});
}

void PsSession::appendLog(const LogLine& l)
{
    if (m_log.size() >= kLogLimit) {
        m_log.remove(0, kLogLimit / 10);
    }
    m_log.push_back(l);
    emit frameAppended(l);
}

void PsSession::clearLog()
{
    m_log.clear();
    emit logCleared();
}

void PsSession::clearSffResults()
{
    m_sffResults.clear();
    emit sffChanged();
}
