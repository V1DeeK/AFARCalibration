#include "VideoProto.h"

#include <QStringList>

namespace video {

namespace {

QByteArray withCrc(QByteArray pdu)
{
    const quint16 c = crc16(pdu);
    pdu.append(static_cast<char>(c & 0xFF));
    pdu.append(static_cast<char>(c >> 8));
    return pdu;
}

void put16(QByteArray& out, quint16 v)
{
    out.append(static_cast<char>(v >> 8));
    out.append(static_cast<char>(v & 0xFF));
}

quint8 at(const QByteArray& b, int i)
{
    return static_cast<quint8>(b.at(i));
}

QString pick(const QStringList& names, int i)
{
    return i >= 0 && i < names.size() ? names.at(i) : QString::number(i);
}

}  // namespace

quint16 crc16(const QByteArray& bytes)
{
    quint16 c = 0xFFFF;
    for (const char ch : bytes) {
        c ^= static_cast<quint8>(ch);
        for (int i = 0; i < 8; ++i) {
            c = (c & 1) ? static_cast<quint16>((c >> 1) ^ 0xA001) : static_cast<quint16>(c >> 1);
        }
    }
    return c;
}

quint32 crc32(const QByteArray& bytes)
{
    quint32 c = 0xFFFFFFFFu;
    for (const char ch : bytes) {
        c ^= static_cast<quint8>(ch);
        for (int i = 0; i < 8; ++i) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return c ^ 0xFFFFFFFFu;
}

QByteArray readFrame(quint8 addr, quint8 fn, quint16 reg, quint16 count)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(addr));
    pdu.append(static_cast<char>(fn));
    put16(pdu, reg);
    put16(pdu, count);
    return withCrc(pdu);
}

QByteArray writeMultiFrame(quint8 addr, quint16 reg, const QList<quint16>& values)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(addr));
    pdu.append(static_cast<char>(0x10));
    put16(pdu, reg);
    put16(pdu, static_cast<quint16>(values.size()));
    pdu.append(static_cast<char>(values.size() * 2));
    for (const quint16 v : values) {
        put16(pdu, v);
    }
    return withCrc(pdu);
}

QByteArray writeSingleFrame(quint8 addr, quint16 reg, quint16 value)
{
    QByteArray pdu;
    pdu.append(static_cast<char>(addr));
    pdu.append(static_cast<char>(6));
    put16(pdu, reg);
    put16(pdu, value);
    return withCrc(pdu);
}

int expectedLength(const QByteArray& b)
{
    if (b.size() < 3) {
        return 0;
    }
    const quint8 fn = at(b, 1);
    if (fn & 0x80) {
        return 5;
    }
    if (fn == 3 || fn == 4) {
        return 5 + at(b, 2);
    }
    if (fn == 6 || fn == 16) {
        return 8;
    }
    return 0;
}

Reply parseReply(const QByteArray& request, const QByteArray& rx)
{
    Reply r;
    if (rx.isEmpty()) {
        return r;
    }
    // Длина и CRC проверяются до любого разбора: ответ с шины — недоверенные байты.
    if (rx.size() < 5 || request.size() < 2) {
        r.kind = ReplyKind::BadFrame;
        return r;
    }
    const quint16 got = static_cast<quint16>(at(rx, static_cast<int>(rx.size()) - 2)
                                             | (at(rx, static_cast<int>(rx.size()) - 1) << 8));
    if (crc16(rx.left(rx.size() - 2)) != got) {
        r.kind = ReplyKind::BadFrame;
        return r;
    }
    if (rx.at(0) != request.at(0)) {
        r.kind = ReplyKind::WrongAddress;
        return r;
    }
    const quint8 fn = at(request, 1);
    if (at(rx, 1) == (fn | 0x80)) {
        r.kind = ReplyKind::Exception;
        r.exception = at(rx, 2);
        return r;
    }
    if (at(rx, 1) != fn) {
        r.kind = ReplyKind::WrongFunction;
        return r;
    }
    if (fn == 3 || fn == 4) {
        const int bytes = at(rx, 2);
        if (bytes % 2 != 0 || rx.size() != 5 + bytes) {
            r.kind = ReplyKind::BadFrame;
            return r;
        }
        for (int i = 0; i < bytes / 2; ++i) {
            r.words.append(static_cast<quint16>((at(rx, 3 + 2 * i) << 8) | at(rx, 4 + 2 * i)));
        }
    } else if (rx.size() != 8) {
        r.kind = ReplyKind::BadFrame;
        return r;
    }
    r.kind = ReplyKind::Ok;
    return r;
}

QList<quint16> w32(quint32 v)
{
    return {static_cast<quint16>(v >> 16), static_cast<quint16>(v & 0xFFFF)};
}

QList<quint16> w64(quint64 v)
{
    return {static_cast<quint16>(v >> 48), static_cast<quint16>((v >> 32) & 0xFFFF),
            static_cast<quint16>((v >> 16) & 0xFFFF), static_cast<quint16>(v & 0xFFFF)};
}

quint32 u32(const QList<quint16>& r, int i)
{
    return i + 1 < r.size() ? (static_cast<quint32>(r.at(i)) << 16) | r.at(i + 1) : 0;
}

quint64 u64(const QList<quint16>& r, int i)
{
    return i + 3 < r.size() ? (static_cast<quint64>(u32(r, i)) << 32) | u32(r, i + 2) : 0;
}

QList<quint16> glyphsFor(const QString& text, bool ascii)
{
    static const QString punct = QStringLiteral("().?;:,'/\"-<>@");
    QList<quint16> out;
    for (const QChar ch : text) {
        const ushort c = ch.unicode();
        if (ascii) {
            out.append(c < 256 && c != 0xFF ? c : 0x3F);
            continue;
        }
        quint16 g = 0x42;
        if (c == ' ') {
            g = 0x00;
        } else if (c >= '1' && c <= '9') {
            g = static_cast<quint16>(c - 0x30);
        } else if (c == '0') {
            g = 0x0A;
        } else if (c >= 'A' && c <= 'Z') {
            g = static_cast<quint16>(0x0B + c - 'A');
        } else if (c >= 'a' && c <= 'z') {
            g = static_cast<quint16>(0x25 + c - 'a');
        } else if (const auto i = punct.indexOf(ch); i >= 0) {
            g = static_cast<quint16>(0x3F + i);
        }
        out.append(g);
    }
    return out;
}

QString stateName(int state)
{
    static const QStringList n{"BOOT", "IDLE", "STAGED", "APPLYING", "APPLIED", "FAULT", "SAFE"};
    return pick(n, state);
}

QString errorName(int code)
{
    static const QStringList n{"OK", "BAD_VALUE", "EPOCH_MISMATCH", "INCOMPLETE", "BUSY", "SPI_VERIFY",
                               "PLL_TIMEOUT", "OSD_NOT_READY", "APPLY_TIMEOUT", "WATCHDOG"};
    return pick(n, code);
}

QString fwStateName(int state)
{
    static const QStringList n{"IDLE", "ERASING", "READY", "VERIFYING", "DONE", "ERROR"};
    return pick(n, state);
}

QString fwErrorName(int code)
{
    static const QStringList n{"OK", "BAD_ARGS", "TOO_BIG", "FLASH_WRITE", "CRC_MISMATCH", "BAD_SEQUENCE",
                               "BAD_KEY", "BAD_IMAGE", "INCOMPLETE"};
    return pick(n, code);
}

QString exceptionText(int code)
{
    switch (code) {
    case 1: return QStringLiteral("недопустимая функция");
    case 2: return QStringLiteral("недопустимый адрес");
    case 3: return QStringLiteral("недопустимое значение");
    case 4: return QStringLiteral("отказ устройства");
    case 6: return QStringLiteral("занято");
    default: return QStringLiteral("?");
    }
}

QString regTitle(int index)
{
    static const QStringList n{
        QStringLiteral("R0 · целое N, автокалибровка"), QStringLiteral("R1 · FRAC1"),
        QStringLiteral("R2 · FRAC2 и MOD2"), QStringLiteral("R3 · сигма-дельта"),
        QStringLiteral("R4 · опора, ток накачки, MUXOUT"), QStringLiteral("R5 · фиксированное"),
        QStringLiteral("R6 · делитель, выходы, ток утечки"), QStringLiteral("R7 · детектор захвата"),
        QStringLiteral("R8 · фиксированное (-3)"), QStringLiteral("R9 · полоса ГУН, тайм-ауты"),
        QStringLiteral("R10 · АЦП калибровки"), QStringLiteral("R11 · фиксированное (-3)"),
        QStringLiteral("R12 · resync")};
    return pick(n, index);
}

QString regMeaning(int index, quint32 w)
{
    const auto f = [w](int shift, int bits) { return (w >> shift) & ((1u << bits) - 1u); };
    static const QStringList pwr{QStringLiteral("−4"), QStringLiteral("−1"), QStringLiteral("+2"),
                                 QStringLiteral("+5")};
    switch (index) {
    case 0:
        return QStringLiteral("INT %1, прескалер %2, AUTOCAL %3")
            .arg(f(4, 16)).arg(f(20, 1) ? QStringLiteral("8/9") : QStringLiteral("4/5")).arg(f(21, 1));
    case 1: return QStringLiteral("FRAC1 %1").arg(f(4, 24));
    case 2: return QStringLiteral("FRAC2 %1, MOD2 %2").arg(f(18, 14)).arg(f(4, 14));
    case 4:
        return QStringLiteral("MUXOUT %1, R %2, CP код %3 (%4 мА ном.), PD %5")
            .arg(f(27, 3)).arg(f(15, 10)).arg(f(10, 4)).arg((f(10, 4) + 1) * 0.3, 0, 'f', 1).arg(f(6, 1));
    case 6:
        return QStringLiteral("делитель /%1, bleed %2, A %3")
            .arg(1u << f(21, 3))
            .arg(f(29, 1) ? QStringLiteral("код %1").arg(f(13, 8)) : QStringLiteral("выкл"))
            .arg(f(6, 1) ? QStringLiteral("вкл %1 дБм").arg(pwr.at(static_cast<int>(f(4, 2)))) : QStringLiteral("выкл"));
    case 7: return f(4, 1) ? QStringLiteral("LD целое N, 4096 циклов") : QStringLiteral("LD дробное N, 4096 циклов");
    case 9:
        return QStringLiteral("VCO band div %1, timeout %2, synth lock %3").arg(f(24, 8)).arg(f(14, 10)).arg(f(4, 5));
    case 10: return QStringLiteral("ADC div %1").arg(f(6, 8));
    default: return {};
    }
}

QString hexBytes(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toHex(' '));
}

FirmwareImage inspectFirmware(const QByteArray& file)
{
    FirmwareImage info;
    if (file.size() < 8) {
        info.error = QStringLiteral("файл слишком короткий");
        return info;
    }
    QByteArray b = file;
    while (b.size() % 4 != 0) {
        b.append(static_cast<char>(0xFF));
    }
    const auto le = [&b](int i) {
        return static_cast<quint32>(at(b, i)) | (static_cast<quint32>(at(b, i + 1)) << 8)
               | (static_cast<quint32>(at(b, i + 2)) << 16) | (static_cast<quint32>(at(b, i + 3)) << 24);
    };
    const quint32 sp = le(0);
    const quint32 rv = le(4);
    const bool vectors = sp > 0x20000000u && sp <= 0x20005000u && (rv & 1u) && rv > 0x08002000u && rv < 0x0800F800u;
    info.summary = QStringLiteral("%1 байт, CRC32 %2")
                       .arg(b.size())
                       .arg(QString::number(crc32(b), 16).toUpper().rightJustified(8, QLatin1Char('0')));
    if (!vectors) {
        info.error = QStringLiteral("это не образ приложения для 0x08002000 (нужен video_receiver.bin)");
    } else if (b.size() > kMaxImageBytes) {
        info.error = QStringLiteral("больше 54 КБ");
    } else {
        info.image = b;
    }
    return info;
}

}  // namespace video
