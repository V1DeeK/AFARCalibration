#include "HeterSession.h"

#include "HeterFrame.h"

#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#ifdef AFAR_HAVE_SERIALPORT
#include <QSerialPort>
#include <QSerialPortInfo>
#endif

namespace {

HeterState stateFromParams(const HeterState& previous, const QByteArray& payload)
{
    HeterState state = previous;
    state.connected = true;
    state.real = true;
    state.regsValid = false;
    if (payload.size() < 14) {
        return state;
    }
    const quint16 word = heter::readU16(payload, 0);
    const quint16 att = heter::readU16(payload, 2);
    const qint16 temp = heter::readI16(payload, 4);
    state.lock = ((word >> 5) & 1) != 0;
    state.fOutHz = heter::readU64(payload, 6);
    state.sw1 = (word & 1) == 0;
    state.sw2 = (word & 1) != 0;
    state.ce = ((word >> 1) & 1) != 0;
    state.enRf = ((word >> 2) & 1) != 0;
    state.aEn = ((word >> 3) & 1) != 0;
    state.bEn = ((word >> 4) & 1) != 0;
    state.aDbm = heter::powerDbm((word >> 8) & 3);
    state.bDbm = heter::powerDbm((word >> 10) & 3);
    state.attDb[0] = (att & 0x3F) / 2.0;
    state.attDb[1] = ((att >> 8) & 0x3F) / 2.0;
    state.tempValid = temp != kHeterTempNa;
    state.tempC = state.tempValid ? temp / 100.0 : 0;
    return state;
}

QString stamp(const QElapsedTimer& clock)
{
    return QStringLiteral("[%1 с] ").arg(clock.elapsed() / 1000.0, 0, 'f', 2);
}

}  // namespace

HeterSession::HeterSession(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<HeterState>();
    qRegisterMetaType<HeterReply>();
    m_poll = new QTimer(this);
    m_poll->setInterval(1000);
    connect(m_poll, &QTimer::timeout, this, &HeterSession::poll);
}

HeterSession::~HeterSession()
{
    closeLink();
}

bool HeterSession::serialSupported()
{
#ifdef AFAR_HAVE_SERIALPORT
    return true;
#else
    return false;
#endif
}

QList<HeterPortChoice> HeterSession::ports()
{
    QList<HeterPortChoice> found;
#ifdef AFAR_HAVE_SERIALPORT
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        const QString name = info.portName();
        const QString blob = (name + QLatin1Char(' ') + info.description() + QLatin1Char(' ') + info.manufacturer())
                                 .toLower();
        // tty.* на macOS открывается только при несущей и вешает подключение.
        if (name.startsWith(QStringLiteral("tty."))) {
            continue;
        }
        if (blob.contains(QStringLiteral("bluetooth")) || blob.contains(QStringLiteral("debug-console"))
            || blob.contains(QStringLiteral("wlan")) || blob.contains(QStringLiteral("incoming-port"))) {
            continue;
        }
#ifdef Q_OS_MAC
        const bool usb = info.hasVendorIdentifier() || blob.contains(QStringLiteral("usb"))
                         || blob.contains(QStringLiteral("serial")) || blob.contains(QStringLiteral("uart"));
        if (!usb) {
            continue;
        }
#endif
        HeterPortChoice choice;
        choice.name = name;
        choice.label = info.description().isEmpty() ? name : name + QStringLiteral(" — ") + info.description();
        found.append(choice);
    }
#endif
    return found;
}

HeterState HeterSession::state() const
{
    QMutexLocker lock(&m_mutex);
    return m_state;
}

bool HeterSession::isOpen() const
{
    return m_open;
}

void HeterSession::publish(const HeterState& state)
{
    {
        QMutexLocker lock(&m_mutex);
        m_state = state;
        m_state.tMs = m_clock.isValid() ? static_cast<double>(m_clock.elapsed()) : 0;
    }
    emit stateUpdated(m_state);
}

void HeterSession::note(const QString& line)
{
    emit logLine(stamp(m_clock) + line);
}

bool HeterSession::bootloaderReply(const QByteArray& payload) const
{
    return payload.size() == 1 && static_cast<quint8>(payload.at(0)) == 0x01;
}

HeterReply HeterSession::transact(quint8 func, const QByteArray& data)
{
    if (QThread::currentThread() == thread()) {
        return transactOnThread(func, data, true);
    }
    HeterReply reply;
    QMetaObject::invokeMethod(
        this, [&] { reply = transactOnThread(func, data, true); }, Qt::BlockingQueuedConnection);
    return reply;
}

HeterReply HeterSession::transactOnThread(quint8 func, const QByteArray& data, bool logFrames, int timeoutMs)
{
    HeterReply reply;
    if (!m_open) {
        reply.text = QStringLiteral("нет связи с гетеродином");
        return reply;
    }
    const QByteArray tx = heter::frame(m_addr, func, data);
    if (m_viaBoard) {
        return transactViaBoard(func, tx, logFrames, timeoutMs);
    }

#ifdef AFAR_HAVE_SERIALPORT
    for (int attempt = 0; attempt < 3; ++attempt) {
        m_port->clear(QSerialPort::Input);
        if (logFrames) {
            note(QStringLiteral("RS-485 команда: ") + heter::hex(tx));
        }
        if (m_port->write(tx) != tx.size() || !m_port->waitForBytesWritten(500)) {
            if (logFrames) {
                note(QStringLiteral("ОШИБКА: порт не принял кадр"));
            }
            reply.text = QStringLiteral("порт не принял кадр");
            continue;
        }
        QByteArray buffer;
        QElapsedTimer wait;
        wait.start();
        heter::ParsedReply parsed;
        while (wait.elapsed() < timeoutMs) {
            const int left = timeoutMs - static_cast<int>(wait.elapsed());
            if (m_port->waitForReadyRead(qBound(1, left, 50)) || m_port->bytesAvailable() > 0) {
                buffer += m_port->readAll();
            }
            parsed = heter::takeReply(buffer, m_addr, func);
            if (parsed.kind == QLatin1String("ok") || parsed.kind == QLatin1String("badlen")) {
                break;
            }
        }
        if (parsed.kind != QLatin1String("ok")) {
            const QString kind = parsed.kind == QLatin1String("badlen") ? QStringLiteral("неверная длина ответа")
                                 : parsed.kind == QLatin1String("foreign") ? QStringLiteral("чужой кадр")
                                                                          : QStringLiteral("ответа нет");
            if (logFrames) {
                note(QStringLiteral("ОШИБКА: ") + kind);
            }
            reply.text = kind;
            continue;
        }
        if (logFrames) {
            note(QStringLiteral("RS-485 ответ: ") + heter::hex(parsed.frame));
        }
        reply.ok = parsed.payload.size() != 1 || static_cast<quint8>(parsed.payload.at(0)) == 0;
        reply.rx = heter::hex(parsed.frame);
        reply.tx = heter::hex(tx);
        reply.text = heter::describeReply(m_addr, parsed.frame);
        if (func == 0x05 && parsed.payload.size() == 14) {
            HeterState next = stateFromParams(state(), parsed.payload);
            next.port = m_portName;
            next.addr = m_addr;
            publish(next);
        }
        reply.state = state();
        if (func == 0x05 && !m_bootRecovery && bootloaderReply(parsed.payload)) {
            m_bootRecovery = true;
            note(QStringLiteral("плата в загрузчике после включения, отправляю сброс"));
            transactOnThread(0x0C, {}, true);
            QThread::msleep(500);
            reply = transactOnThread(0x05, {}, true);
            m_bootRecovery = false;
            return reply;
        }
        if (logFrames) {
            emit commandDone(QString::number(func), reply);
        }
        return reply;
    }
    if (reply.text.isEmpty()) {
        reply.text = QStringLiteral("ответа нет");
    }
    if (logFrames) {
        emit commandDone(QString::number(func), reply);
    }
    return reply;
#else
    Q_UNUSED(logFrames);
    reply.text = QStringLiteral("программа собрана без Qt Serial Port");
    return reply;
#endif
}

HeterWireReply HeterSession::askBoard(const QByteArray& frame, int timeoutMs)
{
    HeterWireReply got;
    if (m_wireHost == nullptr || !m_wire) {
        got.error = QStringLiteral("PS: канал к плате не задан");
        return got;
    }
    QPointer<QObject> host(m_wireHost);
    const auto wire = m_wire;
    if (!QMetaObject::invokeMethod(
            m_wireHost, [&] { got = wire(frame, timeoutMs); }, Qt::BlockingQueuedConnection)
        || host == nullptr) {
        got.error = QStringLiteral("PS: нет связи с платой");
    }
    return got;
}

HeterReply HeterSession::transactViaBoard(quint8 func, const QByteArray& tx, bool logFrames, int timeoutMs)
{
    HeterReply reply;
    reply.tx = heter::hex(tx);
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (logFrames) {
            note(QStringLiteral("RS-485 команда: ") + reply.tx);
        }
        const int waitMs = qMax(timeoutMs, 2000) + 1000;
        const HeterWireReply got = askBoard(tx, waitMs);
        if (got.error.isEmpty() && got.bytes == tx) {
            reply.text = QStringLiteral("гетеродин не ответил по RS-485: плата вернула отправленный кадр");
            if (logFrames) {
                note(QStringLiteral("ОШИБКА: ") + reply.text);
            }
            break;
        }
        if (!got.error.isEmpty()) {
            reply.text = got.error;
            if (logFrames) {
                note(QStringLiteral("ОШИБКА: ") + got.error);
            }
            if (got.error.startsWith(QStringLiteral("PS:"))) {
                break;
            }
            continue;
        }
        QByteArray buffer = got.bytes;
        const heter::ParsedReply parsed = heter::takeReply(buffer, m_addr, func);
        if (parsed.kind != QLatin1String("ok")) {
            const QString kind = parsed.kind == QLatin1String("badlen") ? QStringLiteral("неверная длина ответа")
                                 : parsed.kind == QLatin1String("foreign") ? QStringLiteral("чужой кадр")
                                                                          : QStringLiteral("ответа нет");
            reply.text = kind;
            if (logFrames) {
                note(QStringLiteral("ОШИБКА: ") + kind);
            }
            continue;
        }
        if (logFrames) {
            note(QStringLiteral("RS-485 ответ: ") + heter::hex(parsed.frame));
        }
        reply.ok = parsed.payload.size() != 1 || static_cast<quint8>(parsed.payload.at(0)) == 0;
        reply.rx = heter::hex(parsed.frame);
        reply.text = heter::describeReply(m_addr, parsed.frame);
        if (func == 0x05 && parsed.payload.size() == 14) {
            HeterState next = stateFromParams(state(), parsed.payload);
            next.port = m_portName;
            next.addr = m_addr;
            publish(next);
        }
        reply.state = state();
        if (func == 0x05 && !m_bootRecovery && bootloaderReply(parsed.payload)) {
            m_bootRecovery = true;
            note(QStringLiteral("плата в загрузчике после включения, отправляю сброс"));
            transactOnThread(0x0C, {}, true);
            QThread::msleep(500);
            reply = transactOnThread(0x05, {}, true);
            m_bootRecovery = false;
            return reply;
        }
        if (logFrames) {
            emit commandDone(QString::number(func), reply);
        }
        return reply;
    }
    if (reply.text.isEmpty()) {
        reply.text = QStringLiteral("ответа нет");
    }
    if (logFrames) {
        emit commandDone(QString::number(func), reply);
    }
    return reply;
}

void HeterSession::setBoardWire(QObject* host, std::function<HeterWireReply(const QByteArray&, int)> wire)
{
    m_wireHost = host;
    m_wire = std::move(wire);
}

void HeterSession::openViaBoard(int addr)
{
    releasePort(false);
    if (m_wireHost == nullptr || !m_wire) {
        emit linkFailed(QStringLiteral("канал к плате PS не задан"));
        return;
    }
    m_addr = addr;
    m_portName = QStringLiteral("плата PS");
    m_viaBoard = true;
    m_open = true;
    m_clock.restart();
    note(QStringLiteral("гетеродин через плату PS, адрес %1, чтение get_lo_params")
             .arg(addr, 2, 10, QLatin1Char('0')));
    const HeterReply params = transactOnThread(0x05, {}, true);
    if (!params.ok) {
        const QString reason = params.text.isEmpty() ? QStringLiteral("плата не ответила по RS-485") : params.text;
        releasePort(true);
        emit linkFailed(reason);
        return;
    }
    m_poll->start();
}

void HeterSession::openSerial(const QString& portName, int addr)
{
    releasePort(false);
#ifndef AFAR_HAVE_SERIALPORT
    Q_UNUSED(portName);
    Q_UNUSED(addr);
    emit linkFailed(QStringLiteral("программа собрана без модуля Qt Serial Port"));
    return;
#else
    if (portName.trimmed().isEmpty()) {
        emit linkFailed(QStringLiteral("COM-порт не выбран"));
        return;
    }
    QString name = portName.trimmed();
#ifdef Q_OS_MAC
    if (name.startsWith(QStringLiteral("tty."))) {
        name.replace(0, 4, QStringLiteral("cu."));
    }
#endif
    m_port = new QSerialPort(this);
    bool matched = false;
    for (const QSerialPortInfo& info : QSerialPortInfo::availablePorts()) {
        if (info.portName() == name) {
            m_port->setPort(info);
            matched = true;
            break;
        }
    }
    if (!matched) {
        m_port->setPortName(name);
    }
    m_port->setBaudRate(QSerialPort::Baud115200);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port->open(QIODevice::ReadWrite)) {
        const QString reason = m_port->errorString();
        m_port->deleteLater();
        m_port = nullptr;
        emit linkFailed(reason);
        return;
    }
    m_addr = addr;
    m_portName = name;
    m_open = true;
    m_clock.restart();
    note(QStringLiteral("плата на линии %1, 115200 8N1, адрес %2").arg(m_portName).arg(addr, 2, 10, QLatin1Char('0')));
    const HeterReply params = transactOnThread(0x05, {}, true);
    if (!params.ok) {
        const QString reason = params.text.isEmpty() ? QStringLiteral("плата не ответила по RS-485") : params.text;
        releasePort(true);
        emit linkFailed(reason);
        return;
    }
    m_poll->start();
#endif
}

void HeterSession::closeLink()
{
    releasePort(true);
}

void HeterSession::releasePort(bool notify)
{
    m_poll->stop();
    m_open = false;
    m_viaBoard = false;
#ifdef AFAR_HAVE_SERIALPORT
    if (m_port != nullptr) {
        m_port->close();
        m_port->deleteLater();
        m_port = nullptr;
    }
#endif
    HeterState closed;
    closed.addr = m_addr;
    if (notify) {
        publish(closed);
        return;
    }
    QMutexLocker lock(&m_mutex);
    m_state = closed;
}

void HeterSession::setFrequencyMhz(double mhz)
{
    const qint64 hz = qRound64(mhz * 1e6);
    transactOnThread(0x02, heter::packU64(static_cast<quint64>(hz)), true);
}

void HeterSession::setConfig(int refExternal, int pwrA, int pwrB, bool enA, bool enB, bool enRf, bool enSyn)
{
    const int codeA = heter::powerCode(pwrA);
    const int codeB = heter::powerCode(pwrB);
    if (codeA < 0 || codeB < 0) {
        HeterReply reply;
        reply.text = QStringLiteral("недопустимая мощность выхода");
        emit commandDone(QStringLiteral("cfg"), reply);
        return;
    }
    const quint16 word = static_cast<quint16>((refExternal & 1) | ((enSyn ? 1 : 0) << 1) | ((enRf ? 1 : 0) << 2)
                                              | ((enA ? 1 : 0) << 3) | ((enB ? 1 : 0) << 4)
                                              | (static_cast<quint16>(codeA) << 8) | (static_cast<quint16>(codeB) << 10));
    transactOnThread(0x01, heter::packU16(word), true);
}

void HeterSession::setAttenuators(double att1Db, double att2Db)
{
    const int code1 = qRound(att1Db * 2.0);
    const int code2 = qRound(att2Db * 2.0);
    if (code1 < 0 || code1 > 63 || code2 < 0 || code2 > 63) {
        HeterReply reply;
        reply.text = QStringLiteral("ослабление должно быть 0…31,5 дБ с шагом 0,5");
        emit commandDone(QStringLiteral("att"), reply);
        return;
    }
    QByteArray data(2, 0);
    data[0] = static_cast<char>(code1);
    data[1] = static_cast<char>(code2);
    transactOnThread(0x04, data, true);
}

void HeterSession::setChargePump(int code)
{
    QByteArray data(1, static_cast<char>(qBound(0, code, 15)));
    transactOnThread(0x0D, data, true);
}

void HeterSession::queryParams()
{
    transactOnThread(0x05, {}, true);
}

void HeterSession::queryHealth()
{
    transactOnThread(0x06, {}, true);
}

void HeterSession::poll()
{
    if (!m_open) {
        return;
    }
    transactOnThread(0x05, {}, false);
}

namespace {

int frameLength(const HeterReply& reply)
{
    const QStringList parts = reply.rx.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 3) {
        return -1;
    }
    bool ok = false;
    const int len = parts.at(2).toInt(&ok, 16);
    return ok ? len : -1;
}

quint32 readLe32(const QByteArray& bytes, int offset)
{
    return static_cast<quint8>(bytes.at(offset))
           | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 8)
           | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 16)
           | (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3))) << 24);
}

QByteArray packLe32(quint32 value)
{
    QByteArray out(4, 0);
    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
    return out;
}

QString locateStFlash()
{
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("st-flash"));
    if (!onPath.isEmpty()) {
        return onPath;
    }
    for (const QString& dir : {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")}) {
        const QString candidate = dir + QStringLiteral("/st-flash");
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

}  // namespace

HeterFirmwareInfo HeterSession::inspectFirmware(const QByteArray& fileBytes)
{
    HeterFirmwareInfo info;
    if (fileBytes.size() < 8) {
        info.error = QStringLiteral("файл короче таблицы векторов");
        return info;
    }
    const quint32 stack = readLe32(fileBytes, 0);
    const quint32 entry = readLe32(fileBytes, 4) & ~1u;
    const bool ram = stack > 0x20000000u && stack <= 0x20005000u;
    const bool application = entry >= 0x08001000u && entry < 0x0800FC00u;
    const bool bootloader = entry >= 0x08000000u && entry < 0x08001000u;
    if (!ram || !(application || bootloader)) {
        info.error = QStringLiteral(
            "это не прошивка этой платы: стек должен быть в ОЗУ STM32F103, точка входа — в загрузчике "
            "или в приложении с адреса 0x08001000");
        return info;
    }
    QByteArray image = fileBytes;
    if (image.size() & 1) {
        image.append(char(0xFF));
    }
    constexpr int kAppMax = 0x0800FC00 - 0x08001000;
    if (application && (image.size() < 256 || image.size() > kAppMax)) {
        info.error = QStringLiteral("размер рабочей прошивки %1 Б вне 256…%2").arg(image.size()).arg(kAppMax);
        return info;
    }
    if (bootloader && image.size() > 0x1000) {
        info.error = QStringLiteral("загрузчик больше 4 КБ");
        return info;
    }
    info.image = image;
    info.application = application;
    info.address = application ? 0x08001000u : 0x08000000u;
    info.summary = application ? QStringLiteral("рабочая прошивка, %1 Б, адрес 0x08001000").arg(image.size())
                               : QStringLiteral("загрузчик, %1 Б, адрес 0x08000000. По RS-485 он не пишется.").arg(image.size());
    return info;
}

void HeterSession::flashRs485(const QByteArray& image)
{
    const HeterFirmwareInfo info = inspectFirmware(image);
    if (!info.error.isEmpty()) {
        emit flashFinished(false, info.error);
        return;
    }
    if (!info.application) {
        emit flashFinished(false, QStringLiteral("по RS-485 пишется только рабочая прошивка. Загрузчик — через ST-Link V2."));
        return;
    }
    if (!m_open) {
        emit flashFinished(false, QStringLiteral("нет связи с платой по RS-485"));
        return;
    }
    m_poll->stop();
    const auto fail = [this](const QString& text) {
        if (m_open) {
            m_poll->start();
        }
        emit flashFinished(false, text);
    };

    emit flashProgress(2, QStringLiteral("ищу загрузчик"));
    HeterReply boot = transactOnThread(0x08, {}, true, 200);
    if (frameLength(boot) != 12) {
        if (frameLength(boot) == 1) {
            emit flashProgress(4, QStringLiteral("ответила рабочая прошивка, сбрасываю в загрузчик"));
            const HeterReply reset = transactOnThread(0x0C, {}, true, 200);
            if (!reset.ok) {
                fail(reset.text.isEmpty() ? QStringLiteral("плата не приняла сброс") : reset.text);
                return;
            }
            QThread::msleep(50);
            bool found = false;
            QElapsedTimer hunt;
            hunt.start();
            while (hunt.elapsed() < 1500) {
                boot = transactOnThread(0x08, {}, false, 80);
                if (frameLength(boot) == 12) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                fail(QStringLiteral("загрузчик не ответил после сброса. Один раз запишите его через ST-Link V2."));
                return;
            }
        } else {
            fail(boot.text.isEmpty() ? QStringLiteral("загрузчик не ответил") : boot.text);
            return;
        }
    }

    note(QStringLiteral("загрузчик на линии, пишу %1 Б").arg(info.image.size()));
    const HeterReply begin = transactOnThread(0x09, packLe32(static_cast<quint32>(info.image.size())), true, 250);
    if (!begin.ok) {
        fail(begin.text.isEmpty() ? QStringLiteral("плата не начала обновление") : begin.text);
        return;
    }
    constexpr int kBlock = 192;
    for (int offset = 0; offset < info.image.size(); offset += kBlock) {
        const QByteArray chunk = info.image.mid(offset, kBlock);
        QByteArray data = packLe32(static_cast<quint32>(offset));
        data.append(chunk);
        const HeterReply block = transactOnThread(0x0A, data, false, 250);
        if (!block.ok) {
            fail(QStringLiteral("блок со смещения %1: %2").arg(offset).arg(block.text.isEmpty() ? QStringLiteral("нет ответа") : block.text));
            return;
        }
        const int done = offset + chunk.size();
        emit flashProgress(5 + (90 * done) / info.image.size(),
                           QStringLiteral("записано %1 / %2 Б").arg(done).arg(info.image.size()));
    }
    const quint16 crc = heter::crc16(info.image);
    QByteArray tail = packLe32(static_cast<quint32>(info.image.size()));
    tail.append(static_cast<char>(crc & 0xFF));
    tail.append(static_cast<char>((crc >> 8) & 0xFF));
    const HeterReply end = transactOnThread(0x0B, tail, true, 400);
    if (!end.ok) {
        fail(end.text.isEmpty() ? QStringLiteral("плата не подтвердила образ") : end.text);
        return;
    }
    emit flashProgress(97, QStringLiteral("образ принят, жду запуск прошивки"));
    QThread::msleep(400);
    m_bootRecovery = true;
    const HeterReply params = transactOnThread(0x05, {}, true, 500);
    m_bootRecovery = false;
    if (m_open) {
        m_poll->start();
    }
    if (!params.ok) {
        emit flashFinished(false, QStringLiteral("образ записан, но плата не ответила параметрами: %1").arg(params.text));
        return;
    }
    emit flashProgress(100, QStringLiteral("прошивка запущена"));
    emit flashFinished(true, QStringLiteral("прошивка записана по RS-485 и плата ответила"));
}

void HeterSession::flashStlink(const QString& path, quint32 address)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit flashFinished(false, QStringLiteral("не открыть файл: ") + file.errorString());
        return;
    }
    const HeterFirmwareInfo info = inspectFirmware(file.readAll());
    file.close();
    if (!info.error.isEmpty()) {
        emit flashFinished(false, info.error);
        return;
    }
    if (info.address != address) {
        emit flashFinished(false, QStringLiteral("адрес записи не совпадает с типом файла"));
        return;
    }
    const QString tool = locateStFlash();
    if (tool.isEmpty()) {
        emit flashFinished(false,
                           QStringLiteral("не найден st-flash. Нужна программа stlink и подключённый ST-Link V2."));
        return;
    }
    emit flashProgress(15, QStringLiteral("ST-Link пишет %1 по адресу 0x%2")
                                .arg(info.summary)
                                .arg(address, 8, 16, QLatin1Char('0')));
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    const QString addr = QStringLiteral("0x%1").arg(address, 8, 16, QLatin1Char('0'));
    proc.start(tool,
               {QStringLiteral("--connect-under-reset"), QStringLiteral("--reset"), QStringLiteral("--flash=64k"),
                QStringLiteral("--format"), QStringLiteral("binary"), QStringLiteral("write"), path, addr});
    if (!proc.waitForStarted(3000)) {
        emit flashFinished(false, QStringLiteral("st-flash не запустился"));
        return;
    }
    if (!proc.waitForFinished(120000)) {
        proc.kill();
        proc.waitForFinished(2000);
        emit flashFinished(false, QStringLiteral("st-flash не закончил за 2 минуты"));
        return;
    }
    const QString out = QString::fromLocal8Bit(proc.readAll()).trimmed();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        emit flashFinished(false, out.isEmpty() ? QStringLiteral("st-flash завершился с ошибкой") : out);
        return;
    }
    emit flashProgress(100, QStringLiteral("ST-Link записал образ и перезапустил плату"));
    emit flashFinished(true, out.isEmpty() ? QStringLiteral("прошивка записана через ST-Link V2") : out);
}
