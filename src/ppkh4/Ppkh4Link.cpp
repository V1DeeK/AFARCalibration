#include "Ppkh4Link.h"

#ifdef AFAR_HAVE_SERIALPORT
#include <QSerialPort>
#include <QSerialPortInfo>
#endif

#include <algorithm>

Ppkh4Link::Ppkh4Link(QObject* parent)
    : QObject(parent)
{
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(kCommandTimeoutMs);
    connect(&m_timeout, &QTimer::timeout, this, [this]() { finishCurrent(QStringLiteral("ERR timeout")); });
}

Ppkh4Link::~Ppkh4Link()
{
    // Ответы не вызываем: владелец уже разрушается.
    m_queue.clear();
#ifdef AFAR_HAVE_SERIALPORT
    if (m_port != nullptr) {
        m_port->close();
    }
#endif
}

bool Ppkh4Link::serialSupported()
{
#ifdef AFAR_HAVE_SERIALPORT
    return true;
#else
    return false;
#endif
}

QList<Ppkh4Link::PortInfo> Ppkh4Link::availablePorts()
{
    QList<PortInfo> out;
#ifdef AFAR_HAVE_SERIALPORT
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        PortInfo p;
        p.name = info.portName();
        p.description = info.description();
        p.board = info.hasVendorIdentifier() && info.hasProductIdentifier()
                  && info.vendorIdentifier() == kVendorId && info.productIdentifier() == kProductId;
        out.append(p);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const PortInfo& a, const PortInfo& b) { return a.board && !b.board; });
#endif
    return out;
}

bool Ppkh4Link::open(const QString& portName)
{
    close();
#ifdef AFAR_HAVE_SERIALPORT
    if (portName.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("COM-порт не выбран");
        return false;
    }
    m_port = new QSerialPort(this);
    m_port->setPortName(portName.trimmed());
    // Для USB CDC скорость не важна; 115200 — как в исходной странице.
    m_port->setBaudRate(QSerialPort::Baud115200);
    if (!m_port->open(QIODevice::ReadWrite)) {
        m_lastError = m_port->errorString();
        m_port->deleteLater();
        m_port = nullptr;
        return false;
    }
    m_port->setDataTerminalReady(true);
    connect(m_port, &QSerialPort::readyRead, this, &Ppkh4Link::onReadyRead);
    connect(m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::ResourceError || error == QSerialPort::PermissionError) {
            m_lastError = m_port != nullptr ? m_port->errorString() : QString();
            const QString reason = m_lastError;
            close();
            emit closed(reason);
        }
    });
    m_portName = portName.trimmed();
    m_lastError.clear();
    emit opened();
    return true;
#else
    Q_UNUSED(portName);
    m_lastError = QStringLiteral("программа собрана без модуля Qt Serial Port");
    return false;
#endif
}

void Ppkh4Link::close()
{
    m_timeout.stop();
    dropAll(QStringLiteral("ERR disconnected"));
    m_rx.clear();
#ifdef AFAR_HAVE_SERIALPORT
    if (m_port != nullptr) {
        m_port->disconnect(this);
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
    }
#endif
    m_portName.clear();
}

bool Ppkh4Link::isOpen() const
{
#ifdef AFAR_HAVE_SERIALPORT
    return m_port != nullptr && m_port->isOpen();
#else
    return false;
#endif
}

void Ppkh4Link::send(const QString& command, Reply reply)
{
    if (!isOpen()) {
        if (reply) {
            reply(QStringLiteral("ERR not connected"));
        }
        return;
    }
    m_queue.push_back(Job{command, std::move(reply)});
    if (!m_busy) {
        startNext();
    }
}

void Ppkh4Link::startNext()
{
    if (m_busy || m_queue.empty()) {
        return;
    }
#ifdef AFAR_HAVE_SERIALPORT
    if (m_port == nullptr) {
        return;
    }
    m_busy = true;
    const QString command = m_queue.front().command;
    m_port->write(command.toLatin1() + '\n');
    m_timeout.start();
    emit commandSent(command);
#endif
}

void Ppkh4Link::onReadyRead()
{
#ifdef AFAR_HAVE_SERIALPORT
    if (m_port == nullptr) {
        return;
    }
    m_rx += m_port->readAll();
    qsizetype nl = 0;
    while ((nl = m_rx.indexOf('\n')) >= 0) {
        const QString line = QString::fromLatin1(m_rx.left(nl)).trimmed();
        m_rx.remove(0, nl + 1);
        if (!line.isEmpty()) {
            onLine(line);
        }
    }
#endif
}

void Ppkh4Link::onLine(const QString& line)
{
    emit lineReceived(line);
    const bool err = line.startsWith(QLatin1String("ERR"));
    if (err) {
        emit boardError(line.mid(4));
    }
    // Многострочные ответы (HELP) завершает первая строка OK/ERR, остальные — только в журнал.
    if (m_busy && (err || line.startsWith(QLatin1String("OK")))) {
        finishCurrent(line);
    }
}

void Ppkh4Link::finishCurrent(const QString& line)
{
    m_timeout.stop();
    if (!m_busy || m_queue.empty()) {
        m_busy = false;
        return;
    }
    Job job = std::move(m_queue.front());
    m_queue.pop_front();
    m_busy = false;
    if (job.reply) {
        job.reply(line);
    }
    startNext();
}

void Ppkh4Link::dropAll(const QString& line)
{
    std::deque<Job> jobs;
    jobs.swap(m_queue);
    m_busy = false;
    for (Job& job : jobs) {
        if (job.reply) {
            job.reply(line);
        }
    }
}
