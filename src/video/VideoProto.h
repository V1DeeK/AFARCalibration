#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>

/// Узел VIDEO (модуль видеоприёма Э.468116.00133, V1DeeK/Video_receiver): Modbus RTU 115200 8E1,
/// адрес 3, профиль PF60-HW 0.1 с расширениями VIDEO-EXT и VIDEO-FWUP. Карта регистров —
/// README репозитория Video_receiver; здесь только сборка и разбор кадров, без порта.
namespace video {

inline constexpr quint16 kFwKey = 0xB007;
inline constexpr quint16 kCommitMagic = 0xA55A;
inline constexpr double kIfHz = 480e6;
inline constexpr int kMaxImageBytes = 0xD800;  ///< 54 КБ приложения с 0x08002000

enum Flag : quint32 { FlagPllLock = 1, FlagVideoLoss = 2, FlagOsdReady = 4, FlagWatchdogSafe = 8 };
enum Command : quint16 { CmdApply = 1, CmdSafe = 2, CmdClearFault = 3, CmdOsdWrite = 4 };

[[nodiscard]] quint16 crc16(const QByteArray& bytes);
[[nodiscard]] quint32 crc32(const QByteArray& bytes);

/// Кадры с CRC. fn — 3 (holding) или 4 (input).
[[nodiscard]] QByteArray readFrame(quint8 addr, quint8 fn, quint16 reg, quint16 count);
[[nodiscard]] QByteArray writeMultiFrame(quint8 addr, quint16 reg, const QList<quint16>& values);
[[nodiscard]] QByteArray writeSingleFrame(quint8 addr, quint16 reg, quint16 value);

/// Полная длина ответа по первым трём байтам. 0 — ещё неизвестна.
[[nodiscard]] int expectedLength(const QByteArray& partial);

enum class ReplyKind { Ok, NoReply, BadFrame, WrongAddress, Exception, WrongFunction };
struct Reply {
    ReplyKind kind = ReplyKind::NoReply;
    int exception = 0;
    QList<quint16> words;  ///< регистры ответа на 03/04
};
[[nodiscard]] Reply parseReply(const QByteArray& request, const QByteArray& rx);

[[nodiscard]] QList<quint16> w32(quint32 v);
[[nodiscard]] QList<quint16> w64(quint64 v);
[[nodiscard]] quint32 u32(const QList<quint16>& r, int i);
[[nodiscard]] quint64 u64(const QList<quint16>& r, int i);

/// Коды символов OSD: заводская таблица MAX7456 либо ASCII 1:1.
[[nodiscard]] QList<quint16> glyphsFor(const QString& text, bool ascii);

[[nodiscard]] QString stateName(int state);
[[nodiscard]] QString errorName(int code);
[[nodiscard]] QString fwStateName(int state);
[[nodiscard]] QString fwErrorName(int code);
[[nodiscard]] QString exceptionText(int code);
[[nodiscard]] QString regTitle(int index);
[[nodiscard]] QString regMeaning(int index, quint32 word);
[[nodiscard]] QString hexBytes(const QByteArray& bytes);

struct FirmwareImage {
    QByteArray image;   ///< дополнен 0xFF до кратности 4; пуст, если файл не годится
    QString summary;
    QString error;
};
/// Проверка video_receiver.bin: таблица векторов приложения с 0x08002000 и размер.
[[nodiscard]] FirmwareImage inspectFirmware(const QByteArray& file);

struct Status {
    int mapVersion = 0, moduleType = 0, fwMajor = 0, fwMinor = 0;
    quint32 bootId = 0, appliedSeq = 0, flags = 0, epoch = 0, uptimeMs = 0, acceptedSeq = 0, caps = 0;
    int state = 0, lastError = 0;
    int fwMode = 1;        ///< 1 приложение, 2 загрузчик
    int blVersion = 0;
    bool hasExt = false;   ///< прочитан VIDEO-EXT
    int agcMode = 0, manualAtten = 0, targetMv = 0, rssiMv = 0, attenCode = 0, band = 0, osdStat = 0;
    int rtcPaths = -1;     ///< −1 — прошивка без регистра
    quint32 rtcPd = 0;
    bool hasRtcRead = false;
    quint32 rtcRead = 0;
    int bandMode = -1, cpCode = -1;
    quint64 freqHz = 0, calcHz = 0;
    int rfEnable = 0, power = 0, profile = 1;
    int fwupState = 0, fwupError = 0, appValid = 0;
    quint32 fwupMax = 0, fwupWritten = 0, appSize = 0, appCrc = 0, appVersion = 0;

    [[nodiscard]] QString fwText() const { return QStringLiteral("%1.%2").arg(fwMajor).arg(fwMinor); }
    [[nodiscard]] QString blText() const
    {
        return blVersion ? QStringLiteral("%1.%2").arg(blVersion >> 8).arg(blVersion & 255) : QStringLiteral("нет");
    }
    [[nodiscard]] QString bootText() const { return QString::number(bootId, 16).toUpper().rightJustified(8, QLatin1Char('0')); }
};

struct ApplyRequest {
    double freqHz = 5800e6;
    int profile = 1;
    bool rf = true;
    int power = 3;
    bool sendExt = true;
    int agcMode = 1, attenCode = 0, targetMv = 2500, bandMode = 0;
};

struct TestOptions {
    QString serial;
    bool haveSignal = false;
    double signalMhz = 5800;
    bool haveVideo = false;
    bool asciiFont = false;
};

}  // namespace video

Q_DECLARE_METATYPE(video::Status)
Q_DECLARE_METATYPE(video::ApplyRequest)
Q_DECLARE_METATYPE(video::TestOptions)
