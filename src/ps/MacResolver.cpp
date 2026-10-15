#include "MacResolver.h"

#include <QFile>
#include <QHostAddress>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QProcess>
#include <QRegularExpression>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr int kMaxParallelProbes = 64;
constexpr int kProbeTimeoutMs = 600;
constexpr int kSweepTimeoutMs = 12000;
constexpr int kArpTimeoutMs = 4000;

}  // namespace

MacResolver::MacResolver(QObject* parent)
    : QObject(parent)
    , m_sweepTimeout(new QTimer(this))
{
    m_sweepTimeout->setSingleShot(true);
    m_sweepTimeout->setInterval(kSweepTimeoutMs);
    connect(m_sweepTimeout, &QTimer::timeout, this, &MacResolver::finishSweep);
}

MacResolver::~MacResolver() = default;

QString MacResolver::normalizeMac(const QString& mac)
{
    // macOS печатает октеты без ведущего нуля («0:a:35:2:f3:a8»): дополняем.
    const QStringList parts = mac.trimmed().split(QRegularExpression(QStringLiteral("[-:]")));
    QString hex;
    if (parts.size() == 6) {
        for (const QString& part : parts) {
            if (part.isEmpty() || part.size() > 2) {
                return {};
            }
            hex += part.rightJustified(2, QLatin1Char('0'));
        }
    } else {
        hex = mac.trimmed();
    }
    hex = hex.toUpper();
    static const QRegularExpression hex12(QStringLiteral("^[0-9A-F]{12}$"));
    return hex12.match(hex).hasMatch() ? hex : QString();
}

QString MacResolver::formatMac(const QString& mac)
{
    const QString hex = normalizeMac(mac);
    if (hex.isEmpty()) {
        return mac;
    }
    QStringList parts;
    for (int i = 0; i < 12; i += 2) {
        parts << hex.mid(i, 2);
    }
    return parts.join(QLatin1Char(':'));
}

bool MacResolver::isValidMac(const QString& mac)
{
    return !normalizeMac(mac).isEmpty();
}

QHash<QString, QString> MacResolver::parseArpOutput(const QString& output)
{
    // Windows: «192.168.1.10   00-0a-35-02-f3-a8   динамический»;
    // macOS / Linux: «? (192.168.1.10) at 0:a:35:2:f3:a8 on en0».
    static const QRegularExpression re(QStringLiteral(
        "\\(?(\\d{1,3}(?:\\.\\d{1,3}){3})\\)?\\s+(?:at\\s+)?"
        "([0-9A-Fa-f]{1,2}(?:[-:][0-9A-Fa-f]{1,2}){5})"));
    QHash<QString, QString> table;
    auto it = re.globalMatch(output);
    while (it.hasNext()) {
        const auto m = it.next();
        const QString mac = normalizeMac(m.captured(2));
        if (!mac.isEmpty() && mac != QLatin1String("FFFFFFFFFFFF") && !table.contains(mac)) {
            table.insert(mac, m.captured(1));
        }
    }
    return table;
}

QHash<QString, QString> MacResolver::readArpTable() const
{
    return m_arpReader ? m_arpReader() : readSystemArpTable();
}

QHash<QString, QString> MacResolver::readSystemArpTable()
{
    QProcess proc;
    proc.start(QStringLiteral("arp"), {QStringLiteral("-a")});
    if (!proc.waitForFinished(kArpTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(500);
    }
    QHash<QString, QString> table = parseArpOutput(QString::fromLocal8Bit(proc.readAllStandardOutput()));
#ifdef Q_OS_LINUX
    // Утилиты arp может не быть (net-tools не установлен) — читаем ядро напрямую.
    QFile arpFile(QStringLiteral("/proc/net/arp"));
    if (table.isEmpty() && arpFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        static const QRegularExpression space(QStringLiteral("\\s+"));
        const QStringList lines = QString::fromLatin1(arpFile.readAll()).split(QLatin1Char('\n'));
        for (const QString& line : lines.mid(1)) {
            const QStringList f = line.split(space, Qt::SkipEmptyParts);
            if (f.size() >= 4) {
                const QString mac = normalizeMac(f.at(3));
                if (!mac.isEmpty() && mac != QLatin1String("000000000000")) {
                    table.insert(mac, f.at(0));
                }
            }
        }
    }
#endif
    return table;
}

QString MacResolver::macForIp(const QString& ip) const
{
    const QHash<QString, QString> table = readArpTable();
    for (auto it = table.cbegin(); it != table.cend(); ++it) {
        if (it.value() == ip) {
            return formatMac(it.key());
        }
    }
    return {};
}

void MacResolver::resolve(const QString& mac, quint16 probePort)
{
    cancel();
    m_mac = normalizeMac(mac);
    if (m_mac.isEmpty()) {
        emit failed(QStringLiteral("Некорректный MAC-адрес. Формат: AA:BB:CC:DD:EE:FF"));
        return;
    }
    m_port = probePort;
    m_busy = true;

    emit progress(QStringLiteral("Поиск %1 в таблице ARP…").arg(formatMac(m_mac)));
    const QString cached = readArpTable().value(m_mac);
    if (!cached.isEmpty()) {
        m_busy = false;
        emit progress(QStringLiteral("Плата найдена в таблице ARP: %1").arg(cached));
        emit resolved(cached, formatMac(m_mac));
        return;
    }
    startSweep();
}

void MacResolver::cancel()
{
    m_sweepTimeout->stop();
    m_candidates.clear();
    m_nextCandidate = 0;
    const QSet<QTcpSocket*> probes = m_probes;
    m_probes.clear();
    for (QTcpSocket* socket : probes) {
        socket->setProperty("done", true);
        socket->abort();
        socket->deleteLater();
    }
    m_busy = false;
}

QStringList MacResolver::buildCandidates() const
{
    return m_candidateProvider ? m_candidateProvider() : systemCandidates();
}

QStringList MacResolver::systemCandidates()
{
    QStringList out;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& iface : interfaces) {
        if (!iface.flags().testFlag(QNetworkInterface::IsUp) ||
            !iface.flags().testFlag(QNetworkInterface::IsRunning) ||
            iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            const QHostAddress addr = entry.ip();
            // Подсеть шире /24 перебирать долго — такие пропускаем.
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || entry.prefixLength() < 24) {
                continue;
            }
            const quint32 self = addr.toIPv4Address();
            const quint32 mask = entry.netmask().toIPv4Address();
            if (mask == 0) {
                continue;
            }
            const quint32 network = self & mask;
            const quint32 broadcast = network | ~mask;
            for (quint32 host = network + 1; host < broadcast; ++host) {
                const QString ip = QHostAddress(host).toString();
                if (host != self && !out.contains(ip)) {
                    out.append(ip);
                }
            }
        }
    }
    return out;
}

void MacResolver::startSweep()
{
    m_candidates = buildCandidates();
    m_nextCandidate = 0;
    m_openHosts.clear();
    if (m_candidates.isEmpty()) {
        m_busy = false;
        emit failed(QStringLiteral("Не найдено локальных подсетей для поиска. "
                                   "Проверьте сетевое подключение компьютера."));
        return;
    }
    emit progress(QStringLiteral("Платы нет в таблице ARP. Опрашиваю сеть: %1 адресов…")
                      .arg(m_candidates.size()));
    m_sweepTimeout->start();
    launchNextProbes();
}

void MacResolver::launchNextProbes()
{
    while (m_probes.size() < kMaxParallelProbes && m_nextCandidate < m_candidates.size()) {
        const QString ip = m_candidates.at(m_nextCandidate++);
        auto* socket = new QTcpSocket(this);
        socket->setProxy(QNetworkProxy::NoProxy);
        auto* timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        timeout->setInterval(kProbeTimeoutMs);
        // Любой ответ узла (принял или отказал) наполняет таблицу ARP — этого достаточно.
        connect(socket, &QTcpSocket::connected, this, [this, socket, ip] {
            m_openHosts.append(ip);
            finishProbe(socket);
        });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, socket](QAbstractSocket::SocketError) { finishProbe(socket); });
        connect(timeout, &QTimer::timeout, this, [this, socket] { finishProbe(socket); });
        m_probes.insert(socket);
        socket->connectToHost(ip, m_port);
        timeout->start();
    }
    if (m_probes.isEmpty() && m_nextCandidate >= m_candidates.size()) {
        finishSweep();
    }
}

void MacResolver::finishProbe(QTcpSocket* socket)
{
    if (!socket || socket->property("done").toBool()) {
        return;
    }
    socket->setProperty("done", true);
    m_probes.remove(socket);
    socket->abort();
    socket->deleteLater();
    if (!m_busy) {
        return;
    }
    if (m_nextCandidate < m_candidates.size()) {
        launchNextProbes();
    } else if (m_probes.isEmpty()) {
        finishSweep();
    }
}

void MacResolver::finishSweep()
{
    if (!m_busy) {
        return;
    }
    const QString mac = m_mac;
    cancel();  // останавливает таймер и оставшиеся сокеты, m_busy = false
    const QString ip = readArpTable().value(mac);
    if (ip.isEmpty()) {
        // Что реально есть в сети: узлы с открытым портом платы и их настоящие MAC.
        QVector<QPair<QString, QString>> open;
        QStringList described;
        const QHash<QString, QString> table = readArpTable();
        for (const QString& host : std::as_const(m_openHosts)) {
            QString hostMac;
            for (auto it = table.cbegin(); it != table.cend(); ++it) {
                if (it.value() == host) {
                    hostMac = formatMac(it.key());
                    break;
                }
            }
            open.append({host, hostMac});
            described << (hostMac.isEmpty() ? host : QStringLiteral("%1 (MAC %2)").arg(host, hostMac));
        }
        QString reason = QStringLiteral("Плата с MAC %1 в локальной сети не найдена. ").arg(formatMac(mac));
        if (!open.isEmpty()) {
            reason += QStringLiteral("Но порт %1 принимает подключения у: %2. Скорее всего, MAC платы указан неверно.")
                          .arg(m_port)
                          .arg(described.join(QStringLiteral(", ")));
        } else {
            reason += QStringLiteral("Ни один узел подсети не принял подключение на порт %1: плата выключена, "
                                     "не в этой подсети или программе закрыт доступ к локальной сети.")
                          .arg(m_port);
        }
        emit failed(reason);
        emit portOpenHosts(open);
        return;
    }
    emit progress(QStringLiteral("Плата найдена: %1").arg(ip));
    emit resolved(ip, formatMac(mac));
}
