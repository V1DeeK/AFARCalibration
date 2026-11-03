#include "HeterFrame.h"

namespace heter {

quint16 crc16(const QByteArray& data)
{
    quint16 crc = 0xFFFF;
    for (unsigned char byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? static_cast<quint16>((crc >> 1) ^ 0xA001u) : static_cast<quint16>(crc >> 1);
        }
    }
    return crc;
}

QByteArray frame(int addr, quint8 func, const QByteArray& data)
{
    QByteArray body;
    body.append(static_cast<char>(addr & 0xFF));
    body.append(static_cast<char>(func));
    body.append(static_cast<char>(data.size() & 0xFF));
    body.append(data);
    const quint16 crc = crc16(body);
    body.append(static_cast<char>(crc & 0xFF));
    body.append(static_cast<char>((crc >> 8) & 0xFF));
    return body;
}

QString hex(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toHex(' ')).toUpper();
}

ParsedReply takeReply(QByteArray& buffer, int addr, quint8 func)
{
    const quint8 want = static_cast<quint8>(func + 0x10);
    bool foreign = false;
    while (buffer.size() >= 3) {
        const auto at = [&](int i) { return static_cast<quint8>(buffer.at(i)); };
        if (at(0) != static_cast<quint8>(addr) || at(1) != want) {
            foreign = true;
            buffer.remove(0, 1);
            continue;
        }
        
        const int len = at(2);
        const int need = 3 + len + 2;
        if (buffer.size() < need) {
            break;
        }
        const QByteArray candidate = buffer.left(need);
        buffer.remove(0, need);
        const QByteArray body = candidate.left(candidate.size() - 2);
        const quint16 got = static_cast<quint8>(candidate.at(candidate.size() - 2))
                            | (static_cast<quint16>(static_cast<quint8>(candidate.at(candidate.size() - 1))) << 8);
        if (crc16(body) != got) {
            continue;
        }
        ParsedReply parsed;
        parsed.frame = candidate;
        parsed.payload = candidate.mid(3, len);
        const bool statusOnly = func == 0x05 ? (len == 1 || len == 14) : func == 0x06 ? (len == 1 || len == 2)
                                                                                       : len == 1;
        if (!statusOnly && !(func == 0x05 && len == 14) && !(func == 0x06 && len == 2)) {
            parsed.kind = QStringLiteral("badlen");
            return parsed;
        }
        if ((func == 0x01 || func == 0x02 || func == 0x04 || func == 0x0C || func == 0x0D) && len != 1) {
            parsed.kind = QStringLiteral("badlen");
            return parsed;
        }
        parsed.kind = QStringLiteral("ok");
        return parsed;
    }
    ParsedReply none;
    none.kind = foreign ? QStringLiteral("foreign") : QStringLiteral("timeout");
    return none;
}

QByteArray packU16(quint16 value)
{
    QByteArray out(2, 0);
    out[0] = static_cast<char>(value & 0xFF);
    out[1] = static_cast<char>((value >> 8) & 0xFF);
    return out;
}

QByteArray packU64(quint64 value)
{
    QByteArray out(8, 0);
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
    return out;
}

quint16 readU16(const QByteArray& data, int offset)
{
    return static_cast<quint8>(data.at(offset))
           | (static_cast<quint16>(static_cast<quint8>(data.at(offset + 1))) << 8);
}

qint16 readI16(const QByteArray& data, int offset)
{
    return static_cast<qint16>(readU16(data, offset));
}

quint64 readU64(const QByteArray& data, int offset)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<quint64>(static_cast<quint8>(data.at(offset + i))) << (8 * i);
    }
    return value;
}

QString statusText(quint8 code)
{
    switch (code) {
    case 0x00: return QStringLiteral("Успешно");
    case 0x01: return QStringLiteral("Команда не поддерживается");
    case 0x02: return QStringLiteral("Неверная длина поля Data");
    case 0x03: return QStringLiteral("Неверное значение CRC");
    case 0x04: return QStringLiteral("Недопустимое значение или ненулевые резервные биты");
    case 0x05: return QStringLiteral("Устройство занято");
    case 0x06: return QStringLiteral("Ошибка записи во флеш");
    case 0x07: return QStringLiteral("Образ прошивки не прошёл проверку");
    default: return QStringLiteral("?");
    }
}

int powerDbm(int code)
{
    static constexpr int kDbm[4] = {-4, -1, 2, 5};
    return kDbm[code & 3];
}

int powerCode(int dbm)
{
    switch (dbm) {
    case -4: return 0;
    case -1: return 1;
    case 1:
    case 2: return 2;
    case 5: return 3;
    default: return -1;
    }
}

QString describeReply(int addr, const QByteArray& rx)
{
    if (rx.size() < 5) {
        return QStringLiteral("короткий ответ");
    }
    const int len = static_cast<quint8>(rx.at(2));
    const QByteArray data = rx.mid(3, len);
    const quint8 func = static_cast<quint8>(rx.at(1));
    if (len == 1 && !data.isEmpty()) {
        const auto code = static_cast<quint8>(data.at(0));
        return QStringLiteral("статус %1: %2")
            .arg(QString::number(code, 16).rightJustified(2, QLatin1Char('0')).toUpper(), statusText(code));
    }
    if (func == 0x15 && addr >= 1 && addr <= 2 && data.size() >= 14) {
        const quint16 word = readU16(data, 0);
        const quint16 att = readU16(data, 2);
        const qint16 temp = readI16(data, 4);
        const quint64 freq = readU64(data, 6);
        const QString tempText = temp == kHeterTempNa
                                     ? QStringLiteral("нет данных")
                                     : QString::number(temp / 100.0, 'f', 2) + QStringLiteral(" °C");
        return QStringLiteral("REF=%1 EN_SYN=%2 EN_RF=%3 EN_A=%4 EN_B=%5 LOCK=%6 PWR_A=%7 дБм PWR_B=%8 дБм\n"
                              "ATT_1=%9 дБ ATT_2=%10 дБ   температура: %11\nчастота: %12 Гц")
            .arg(word & 1)
            .arg((word >> 1) & 1)
            .arg((word >> 2) & 1)
            .arg((word >> 3) & 1)
            .arg((word >> 4) & 1)
            .arg((word >> 5) & 1)
            .arg(powerDbm((word >> 8) & 3))
            .arg(powerDbm((word >> 10) & 3))
            .arg((att & 0x3F) / 2.0, 0, 'f', 1)
            .arg(((att >> 8) & 0x3F) / 2.0, 0, 'f', 1)
            .arg(tempText)
            .arg(freq);
    }
    if (func == 0x16 && data.size() >= 2 && addr >= 1 && addr <= 2) {
        const quint16 word = readU16(data, 0);
        const auto yes = [](bool bit) { return bit ? QStringLiteral("да") : QStringLiteral("нет"); };
        return QStringLiteral("LOCK: %1   температура < 60 °C: %2   < 80 °C: %3")
            .arg(yes(word & 1), yes((word >> 1) & 1), yes((word >> 2) & 1));
    }
    return QStringLiteral("данные: ") + hex(data);
}

}  // namespace heter
