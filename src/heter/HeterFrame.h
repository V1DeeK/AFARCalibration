#pragma once

#include "HeterTypes.h"

#include <QString>

/// Кадр RS-485 v3 гетеродина (V1DeeK/Heterodyne, proto.h / rs485_tool.py).
/// [Addr][Func][Len][Data][CRC16/MODBUS lo][CRC16 hi], поля младшим байтом вперёд.
namespace heter {

quint16 crc16(const QByteArray& data);
QByteArray frame(int addr, quint8 func, const QByteArray& data = {});
QString hex(const QByteArray& bytes);

/// Ответ с нужным адресом и функцией+0x10. kind: ok, badlen, timeout, foreign, crc.
struct ParsedReply {
    QString kind;
    QByteArray frame;
    QByteArray payload;
};

ParsedReply takeReply(QByteArray& buffer, int addr, quint8 func);

QByteArray packU16(quint16 value);
QByteArray packU64(quint64 value);
quint16 readU16(const QByteArray& data, int offset);
qint16 readI16(const QByteArray& data, int offset);
quint64 readU64(const QByteArray& data, int offset);

QString statusText(quint8 code);
QString describeReply(int addr, const QByteArray& rx);
int powerDbm(int code);
int powerCode(int dbm);

}  // namespace heter
