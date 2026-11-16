// Кадры Modbus RTU узла VIDEO (модуль видеоприёма, V1DeeK/Video_receiver): сборка, разбор, битые ответы.
#include "video/VideoProto.h"

#include <catch2/catch_test_macros.hpp>

using namespace video;

namespace {

QByteArray hex(const char* text)
{
    return QByteArray::fromHex(text);
}

QByteArray reply(const QByteArray& body)
{
    QByteArray out = body;
    const quint16 c = crc16(body);
    out.append(static_cast<char>(c & 0xFF));
    out.append(static_cast<char>(c >> 8));
    return out;
}

}  // namespace

TEST_CASE("VIDEO: CRC16 Modbus и CRC32", "[video]")
{
    // Известный кадр Modbus: 01 03 00 00 00 0A → CRC C5 CD.
    CHECK(crc16(hex("01030000000A")) == 0xCDC5);
    CHECK(crc32(QByteArray("123456789")) == 0xCBF43926u);
    CHECK(crc32(QByteArray()) == 0u);
}

TEST_CASE("VIDEO: кадры запроса совпадают с примером README", "[video]")
{
    // 03 06 0008 A55A — COMMIT; 03 04 0000 0014 — чтение статуса.
    CHECK(writeSingleFrame(3, 0x0008, kCommitMagic).left(6) == hex("03060008A55A"));
    CHECK(readFrame(3, 4, 0x0000, 0x14).left(6) == hex("030400000014"));
    // 5 800 000 000 Гц, RF вкл., +5 дБм, профиль 1.
    QList<quint16> synth = w64(5800000000ULL);
    synth += {1, 3, 1, 0};
    CHECK(writeMultiFrame(3, 0x0100, synth).left(23) == hex("0310010000081000000001" "59B4FA00" "0001000300010000"));
    CHECK(writeMultiFrame(3, 0x0100, synth).size() == 25);
}

TEST_CASE("VIDEO: разбор корректного ответа", "[video]")
{
    const QByteArray request = readFrame(3, 4, 0x0400, 2);
    const Reply r = parseReply(request, reply(hex("030404" "0001" "0B54")));
    REQUIRE(r.kind == ReplyKind::Ok);
    REQUIRE(r.words.size() == 2);
    CHECK(r.words[0] == 1);
    CHECK(r.words[1] == 0x0B54);
    CHECK(u32(r.words, 0) == 0x00010B54u);

    const QByteArray write = writeSingleFrame(3, 8, kCommitMagic);
    CHECK(parseReply(write, write).kind == ReplyKind::Ok);
}

TEST_CASE("VIDEO: битые и чужие ответы отбрасываются", "[video]")
{
    const QByteArray request = readFrame(3, 4, 0x0400, 2);
    QByteArray good = reply(hex("030404" "0001" "0B54"));
    CHECK(parseReply(request, {}).kind == ReplyKind::NoReply);
    CHECK(parseReply(request, good.left(4)).kind == ReplyKind::BadFrame);  // короче минимума
    QByteArray flipped = good;
    flipped[4] = static_cast<char>(flipped[4] ^ 0x01);
    CHECK(parseReply(request, flipped).kind == ReplyKind::BadFrame);  // CRC
    CHECK(parseReply(request, reply(hex("050404" "0001" "0B54"))).kind == ReplyKind::WrongAddress);
    CHECK(parseReply(request, reply(hex("030304" "0001" "0B54"))).kind == ReplyKind::WrongFunction);
    // Счётчик байт врёт о длине: CRC верен, но кадр не принимается.
    CHECK(parseReply(request, reply(hex("030406" "0001" "0B54"))).kind == ReplyKind::BadFrame);
    CHECK(parseReply(request, reply(hex("030403" "0001" "0B"))).kind == ReplyKind::BadFrame);
    const Reply ex = parseReply(request, reply(hex("038403")));
    CHECK(ex.kind == ReplyKind::Exception);
    CHECK(ex.exception == 3);
}

TEST_CASE("VIDEO: длина ответа по заголовку", "[video]")
{
    CHECK(expectedLength(hex("0304")) == 0);
    CHECK(expectedLength(hex("030428")) == 45);
    CHECK(expectedLength(hex("038403")) == 5);
    CHECK(expectedLength(hex("031000")) == 8);
    CHECK(expectedLength(hex("030600")) == 8);
}

TEST_CASE("VIDEO: символы OSD", "[video]")
{
    CHECK(glyphsFor(QStringLiteral("A0 9z"), false) == QList<quint16>{0x0B, 0x0A, 0x00, 0x09, 0x3E});
    CHECK(glyphsFor(QStringLiteral("(@"), false) == QList<quint16>{0x3F, 0x4C});
    CHECK(glyphsFor(QStringLiteral("Я"), false) == QList<quint16>{0x42});
    CHECK(glyphsFor(QStringLiteral("A Я"), true) == QList<quint16>{0x41, 0x20, 0x3F});
}

TEST_CASE("VIDEO: проверка образа прошивки", "[video]")
{
    QByteArray image(1022, '\0');
    // SP = 0x20005000, вектор сброса 0x08002101 (Thumb).
    const quint32 sp = 0x20005000u;
    const quint32 rv = 0x08002101u;
    for (int i = 0; i < 4; ++i) {
        image[i] = static_cast<char>(sp >> (8 * i));
        image[4 + i] = static_cast<char>(rv >> (8 * i));
    }
    const FirmwareImage ok = inspectFirmware(image);
    CHECK(ok.error.isEmpty());
    CHECK(ok.image.size() == 1024);  // дополнен 0xFF до кратности 4
    CHECK(static_cast<quint8>(ok.image.at(1023)) == 0xFF);

    QByteArray full = image;
    full[4] = 0x01;
    full[5] = 0x01;  // вектор 0x08000101 — это загрузчик/полный образ, не приложение
    full[6] = 0x00;
    CHECK(inspectFirmware(full).image.isEmpty());
    CHECK(inspectFirmware(QByteArray(4, '\0')).image.isEmpty());
    QByteArray big = image;
    big.resize(kMaxImageBytes + 4);
    CHECK(inspectFirmware(big).image.isEmpty());
}

TEST_CASE("VIDEO: расшифровка регистров ADF4355", "[video]")
{
    CHECK(stateName(6) == QStringLiteral("SAFE"));
    CHECK(errorName(6) == QStringLiteral("PLL_TIMEOUT"));
    CHECK(errorName(42) == QStringLiteral("42"));
    // R0: INT = 106 в битах 19…4, AUTOCAL = 1.
    CHECK(regMeaning(0, (1u << 21) | (106u << 4)) == QStringLiteral("INT 106, прескалер 4/5, AUTOCAL 1"));
}
