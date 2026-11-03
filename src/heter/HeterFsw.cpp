#include "HeterFsw.h"

#include <QHostAddress>
#include <QTcpSocket>
#include <QUuid>

#include <QImage>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

[[noreturn]] void fail(const QString& text)
{
    throw std::runtime_error(text.toStdString());
}

QString quoteChannel(QString name)
{
    if (name.contains(QLatin1Char(' ')) && !name.startsWith(QLatin1Char('\'')) && !name.startsWith(QLatin1Char('"'))) {
        name.replace(QLatin1Char('\''), QStringLiteral("''"));
        return QStringLiteral("'") + name + QStringLiteral("'");
    }
    return name;
}

}  // namespace

QString HeterFsw::identify(const QString& ip, const QString& device)
{
    QHostAddress address;
    if (!address.setAddress(ip) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        fail(QStringLiteral("Укажите IPv4-адрес прибора"));
    }
    const QList<quint16> ports = device == QLatin1String("psu") ? QList<quint16>{5555, 5025} : QList<quint16>{5025};
    QStringList errors;
    for (quint16 port : ports) {
        QTcpSocket sock;
        sock.connectToHost(address, port);
        if (!sock.waitForConnected(3000)) {
            errors << QStringLiteral("%1: %2").arg(port).arg(sock.errorString());
            continue;
        }
        sock.write("*IDN?\n");
        if (!sock.waitForBytesWritten(3000)) {
            errors << QStringLiteral("%1: %2").arg(port).arg(sock.errorString());
            continue;
        }
        if (!sock.waitForReadyRead(3000)) {
            errors << QStringLiteral("%1: пустой ответ").arg(port);
            continue;
        }
        const QString line = QString::fromLatin1(sock.readLine(1024)).trimmed();
        if (line.isEmpty()) {
            errors << QStringLiteral("%1: пустой ответ").arg(port);
            continue;
        }
        return QStringLiteral("%1:%2 — %3").arg(ip).arg(port).arg(line);
    }
    fail(ip + QStringLiteral(" — ") + errors.join(QStringLiteral(", ")));
}

void HeterFsw::retune(const QString& ip, double centerHz)
{
    QHostAddress address;
    if (!address.setAddress(ip) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        fail(QStringLiteral("Укажите IPv4-адрес анализатора"));
    }
    if (!(centerHz >= 1e6 && centerHz <= 6.7e9)) {
        fail(QStringLiteral("частота для анализатора вне диапазона"));
    }
    QTcpSocket sock;
    sock.connectToHost(address, 5025);
    if (!sock.waitForConnected(800)) {
        fail(QStringLiteral("анализатор не ответил: ") + sock.errorString());
    }
    auto send = [&](const QString& text) {
        const QByteArray line = text.toLatin1() + '\n';
        if (sock.write(line) != line.size() || !sock.waitForBytesWritten(1500)) {
            fail(QStringLiteral("FSW не принял команду ") + text);
        }
    };
    auto readLine = [&] {
        QByteArray buf;
        while (!buf.contains('\n')) {
            if (!sock.waitForReadyRead(2000)) {
                fail(QStringLiteral("FSW не ответил"));
            }
            buf += sock.readAll();
        }
        return QString::fromLatin1(buf.left(buf.indexOf('\n'))).trimmed();
    };
    send(QStringLiteral("*IDN?"));
    const QString idn = readLine().toUpper();
    if (!idn.contains(QLatin1String("ROHDE")) || !idn.contains(QLatin1String("FSW")) || idn.contains(QLatin1String("FSWX"))) {
        fail(QStringLiteral("на этом адресе не анализатор FSW: ") + idn);
    }
    send(QStringLiteral("FREQ:CENT %1").arg(centerHz, 0, 'f', 0));
    send(QStringLiteral("INIT:CONT ON"));
    send(QStringLiteral("SYST:DISP:UPD ON"));
    send(QStringLiteral("*OPC?"));
    if (readLine() != QLatin1String("1")) {
        fail(QStringLiteral("FSW не подтвердил перестройку"));
    }
    send(QStringLiteral("SYST:ERR?"));
    const QString error = readLine();
    const QString code = error.section(QLatin1Char(','), 0, 0).trimmed();
    if (code != QLatin1String("0") && code != QLatin1String("+0")) {
        fail(QStringLiteral("FSW: ") + error);
    }
}

HeterFsw::HeterFsw(const QString& ip)
{
    QHostAddress address;
    if (!address.setAddress(ip) || address.protocol() != QAbstractSocket::IPv4Protocol) {
        fail(QStringLiteral("Укажите IPv4-адрес прибора"));
    }
    m_channel = QStringLiteral("Heter_") + QUuid::createUuid().toString(QUuid::Id128).left(10);
    m_png = QStringLiteral("C:\\R_S\\INSTR\\USER\\") + m_channel + QStringLiteral(".png");
    m_sock = std::make_unique<QTcpSocket>();
    m_sock->connectToHost(address, 5025);
    if (!m_sock->waitForConnected(6000)) {
        fail(QStringLiteral("нет связи с FSW: ") + m_sock->errorString());
    }
    m_sock->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    try {
        m_idn = query(QStringLiteral("*IDN?"));
        const QStringList parts = m_idn.toUpper().split(QLatin1Char(','));
        if (parts.size() < 2 || !parts.at(0).contains(QLatin1String("ROHDE")) || !parts.at(1).startsWith(QLatin1String("FSW"))
            || parts.at(1).startsWith(QLatin1String("FSWX"))) {
            fail(QStringLiteral("Требуется анализатор семейства R&S FSW. Ответ: ") + m_idn);
        }
        check();
        m_previous = query(QStringLiteral("INST:SEL?"));
        command(QStringLiteral("INST:CRE SAN,'%1'").arg(m_channel));
        wait();
        check();
        m_ready = true;
        command(QStringLiteral("INIT:CONT OFF;:TRIG:SOUR IMM;:UNIT:POW DBM"));
        command(QStringLiteral("DISP:TRAC:Y:RLEV 20DBM;:INP:ATT 30;:SWE:POIN 1001"));
        command(QStringLiteral("BAND:VID:AUTO ON;:SWE:TIME:AUTO ON;:FORM ASC;:CALC:MARK1:STAT ON"));
        command(QStringLiteral("SYST:DISP:UPD ON"));
        command(QStringLiteral("HCOP:DEV:LANG PNG"));
        command(QStringLiteral("MMEM:NAME '%1'").arg(m_png));
        wait();
        check();
    } catch (...) {
        m_ready = false;
        throw;
    }
}

HeterFsw::~HeterFsw()
{
    if (!m_ready || m_sock == nullptr) {
        return;
    }
    try {
        cleanup(m_previous);
    } catch (...) {
    }
}

void HeterFsw::cleanup(const QString& previous)
{
    command(QStringLiteral("INST:SEL ") + quoteChannel(previous));
    command(QStringLiteral("INST:DEL '%1'").arg(m_channel));
    if (m_hasPng) {
        command(QStringLiteral("MMEM:DEL '%1'").arg(m_png));
    }
    wait();
    check();
}

void HeterFsw::command(const QString& text)
{
    const QByteArray line = text.toLatin1() + '\n';
    if (m_sock->write(line) != line.size() || !m_sock->waitForBytesWritten(5000)) {
        fail(QStringLiteral("FSW не принял команду ") + text);
    }
}

QByteArray HeterFsw::readBytes(int count)
{
    while (m_buf.size() < count) {
        if (!m_sock->waitForReadyRead(20000)) {
            fail(QStringLiteral("Неполный ответ FSW"));
        }
        m_buf += m_sock->readAll();
        if (m_buf.size() > 8 * 1024 * 1024) {
            fail(QStringLiteral("Слишком длинный ответ FSW"));
        }
    }
    const QByteArray out = m_buf.left(count);
    m_buf.remove(0, count);
    return out;
}

QString HeterFsw::query(const QString& text)
{
    command(text);
    for (;;) {
        while (!m_buf.contains('\n')) {
            if (!m_sock->waitForReadyRead(20000)) {
                fail(QStringLiteral("Неполный ответ FSW на ") + text);
            }
            m_buf += m_sock->readAll();
            if (m_buf.size() > 600000 && !m_buf.contains('\n')) {
                fail(QStringLiteral("Слишком длинный ответ FSW на ") + text);
            }
        }
        const int nl = m_buf.indexOf('\n');
        const QByteArray line = m_buf.left(nl + 1);
        m_buf.remove(0, nl + 1);
        if (line == "\n" || line == "\r\n") {
            continue;
        }
        return QString::fromLatin1(line).trimmed();
    }
}

void HeterFsw::wait()
{
    if (query(QStringLiteral("*OPC?")) != QLatin1String("1")) {
        fail(QStringLiteral("FSW не подтвердил завершение операции"));
    }
}

void HeterFsw::check()
{
    const QString error = query(QStringLiteral("SYST:ERR?"));
    const QString code = error.section(QLatin1Char(','), 0, 0).trimmed();
    if (code != QLatin1String("0") && code != QLatin1String("+0")) {
        fail(QStringLiteral("FSW: ") + error);
    }
}

std::pair<double, double> HeterFsw::sweep(double centerHz, double spanHz, double rbwHz)
{
    command(QStringLiteral("FREQ:CENT %1;:FREQ:SPAN %2;:BAND:RES %3")
                .arg(centerHz, 0, 'f', 0)
                .arg(spanHz, 0, 'f', 0)
                .arg(rbwHz, 0, 'f', 0));
    command(QStringLiteral("INIT:IMM"));
    wait();
    command(QStringLiteral("CALC:MARK1:MAX"));
    const double peak = query(QStringLiteral("CALC:MARK1:X?")).toDouble();
    const double level = query(QStringLiteral("CALC:MARK1:Y?")).toDouble();
    check();
    if (!(std::abs(peak - centerHz) <= spanHz * 0.51 && level > -250.0 && level < 100.0)) {
        fail(QStringLiteral("FSW вернул недопустимый маркер"));
    }
    return {peak, level};
}

HeterPeak HeterFsw::measure(double centerHz, double spanHz)
{
    auto [peak, level] = sweep(centerHz, spanHz, 100000);
    const QString traceText = query(QStringLiteral("TRAC:DATA? TRACE1"));
    std::vector<double> trace;
    for (const QString& part : traceText.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool ok = false;
        const double value = part.toDouble(&ok);
        if (!ok || !std::isfinite(value) || !(value > -250.0 && value < 100.0)) {
            fail(QStringLiteral("Некорректная трасса FSW"));
        }
        trace.push_back(value);
    }
    if (trace.size() < 3) {
        fail(QStringLiteral("Некорректная трасса FSW"));
    }
    std::sort(trace.begin(), trace.end());
    const double floor = trace.size() % 2 == 1 ? trace[trace.size() / 2]
                                               : (trace[trace.size() / 2 - 1] + trace[trace.size() / 2]) / 2.0;
    const double contrast = level - floor;
    double usedSpan = spanHz;
    double rbw = 100000;
    if (contrast >= 10.0) {
        usedSpan = 2000000;
        rbw = 10000;
        const auto refined = sweep(peak, usedSpan, rbw);
        peak = refined.first;
        level = refined.second;
    }
    HeterPeak result;
    result.peakHz = peak;
    result.levelDbm = level;
    result.contrastDb = contrast;
    result.spanHz = usedSpan;
    result.rbwHz = rbw;
    result.signal = contrast >= 10.0;
    return result;
}

QByteArray HeterFsw::screenshot()
{
    command(QStringLiteral("HCOP:IMM"));
    wait();
    check();
    m_hasPng = true;
    command(QStringLiteral("MMEM:DATA? '%1'").arg(m_png));
    if (readBytes(1) != "#") {
        fail(QStringLiteral("FSW не передал двоичный блок снимка"));
    }
    const int digits = QString::fromLatin1(readBytes(1)).toInt();
    if (digits <= 0) {
        fail(QStringLiteral("FSW не указал длину снимка"));
    }
    const int size = QString::fromLatin1(readBytes(digits)).toInt();
    if (size <= 8 || size > 32 * 1024 * 1024) {
        fail(QStringLiteral("Недопустимый размер снимка"));
    }
    const QByteArray data = readBytes(size);
    QImage image;
    if (!image.loadFromData(data, "PNG") || image.isNull()) {
        fail(QStringLiteral("FSW вернул снимок другого формата"));
    }
    return data;
}
