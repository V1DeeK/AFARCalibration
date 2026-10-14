#include "PsSession.h"

#include "MacResolver.h"

#include <QSettings>

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

}  // namespace

PsSession::PsSession(QObject* parent)
    : QObject(parent)
    , m_link(new PsLink(this))
    , m_resolver(new MacResolver(this))
{
    qRegisterMetaType<PsSession::LogLine>();
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

    connect(m_resolver, &MacResolver::progress, this, [this](const QString& text) {
        m_searchText = text;
        emit statusChanged();
    });
    connect(m_resolver, &MacResolver::failed, this, [this](const QString& reason) {
        m_searchText.clear();
        m_lastError = reason;
        emit message(reason);
        emit statusChanged();
    });
    connect(m_resolver, &MacResolver::resolved, this, [this](const QString& ip, const QString& mac) {
        m_searchText.clear();
        // Оператор успел нажать «Отключиться» — найденный адрес только запоминаем.
        QSettings s;
        s.setValue(kGroup + QStringLiteral("host"), ip);
        emit hostResolved(ip);
        emit message(QStringLiteral("Плата %1 найдена по MAC: %2").arg(mac, ip));
        if (!s.value(kGroup + QStringLiteral("auto_connect"), false).toBool() || m_seriesActive) {
            emit statusChanged();
            return;
        }
        if (m_link->isReady() && m_link->host() == ip) {
            emit statusChanged();
            return;
        }
        const bool searched = m_macSearchDone;
        m_link->connectToPs(ip, port(), telemetryPort());
        m_macSearchDone = searched;
        emit statusChanged();
    });
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
        if (m.isEmpty()) {
            refuse(QStringLiteral("Укажите IP-адрес или MAC-адрес платы"));
            return false;
        }
        return findByMac(m, port, telemetryPort);
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
    m_resolver->cancel();
    m_resolver->resolve(mac, port);
    emit statusChanged();
    return true;
}

void PsSession::startLink(const QString& host, quint16 port, quint16 telemetryPort)
{
    m_resolver->cancel();
    m_searchText.clear();
    m_macSearchDone = false;
    m_link->connectToPs(host, port, telemetryPort);
}

void PsSession::disconnectFromPs()
{
    QSettings().setValue(kGroup + QStringLiteral("auto_connect"), false);
    m_reconnectAfterSeries = false;
    m_resolver->cancel();
    m_searchText.clear();
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
    } else if (MacResolver::isValidMac(mac())) {
        findByMac(mac(), port(), telemetryPort());
    }
}

bool PsSession::searching() const
{
    return m_resolver->busy();
}

bool PsSession::busy() const
{
    return m_resolver->busy() || m_link->isTrying();
}

// Адрес не ответил — ищем плату по MAC: IP у неё мог смениться. Один раз на
// попытку подключения, дальше PsLink переподключается сам.
void PsSession::searchByMac()
{
    const QString m = mac();
    if (m_macSearchDone || m_resolver->busy() || !MacResolver::isValidMac(m) || !autoConnect()) {
        return;
    }
    m_macSearchDone = true;
    emit message(QStringLiteral("%1 не отвечает, ищу плату по MAC %2").arg(m_link->host(), m));
    m_resolver->resolve(m, m_link->port());
}

QString PsSession::statusText() const
{
    if (m_seriesActive) {
        return QStringLiteral("идёт серия: PS управляет оркестратор");
    }
    if (m_resolver->busy()) {
        return m_searchText.isEmpty() ? QStringLiteral("поиск платы по MAC…") : m_searchText;
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
        m_state.clear();
        emit modeChanged();
        if (!m_link->lastError().isEmpty()) {
            searchByMac();
        }
        break;
    case PsLink::State::Ready:
        m_macSearchDone = false;
        m_lastError.clear();
        m_link->sendCommand(ps::msg::GetState);
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
        m_resolver->cancel();
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

void PsSession::onResponse(const ps::Envelope& r, ps::Status status)
{
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
    } else if (r.msg_type == ps::msg::SetMode) {
        ps::Mode mode{};
        const auto it = r.payload.find("mode");
        if (it != r.payload.end() && it->is_string() && ps::parseMode(it->get<std::string>(), mode)) {
            m_state.clear();
            setMode(mode, QString());
        }
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
    setMode(t.mode, QString::fromStdString(t.state));
    emit telemetryChanged();
}

void PsSession::onFrame(bool outgoing, const QString& channel, const QString& line)
{
    LogLine l{QDateTime::currentDateTime(), outgoing, channel, line.trimmed()};
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
