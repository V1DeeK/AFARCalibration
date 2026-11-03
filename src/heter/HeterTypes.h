#pragma once

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QPair>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>

/// Адаптер USB-RS485, который можно открыть.
struct HeterPortChoice {
    QString name;
    QString label;
};

/// Состояние гетеродина, как его показывает интерфейс V1DeeK/Heterodyne:
/// то, что прочитано с платы кадром «параметры».
struct HeterState {
    bool connected = false;
    bool real = false;
    int addr = 1;
    QString port;
    bool lock = false;
    quint64 fOutHz = 0;
    quint64 fVcoHz = 0;
    int div = 1;
    bool sw1 = true;
    bool sw2 = false;
    bool ce = false;
    bool enRf = false;
    bool aEn = false;
    bool bEn = false;
    int aDbm = 5;
    int bDbm = 5;
    double attDb[2] = {31.5, 31.5};
    double tempC = 25.1;
    bool tempValid = true;
    bool adfInited = false;
    bool pd = false;
    int cp = 2;
    std::array<quint32, 13> regs{};
    bool regsValid = false;
    QList<QPair<int, quint32>> wrInit;
    QList<QPair<int, quint32>> wrLast;
    QString wrLastKind;
    double tMs = 0;
};

struct HeterReply {
    bool ok = false;
    QString text;
    QString tx;
    QString rx;
    QStringList events;
    HeterState state;
};

struct HeterRunStatus {
    QString id;
    QString status = QStringLiteral("idle");
    int done = 0;
    int total = 0;
    QString detail;
    QString port;
    QString pdfPath;
    QString lastLine;
};

Q_DECLARE_METATYPE(HeterState)
Q_DECLARE_METATYPE(HeterReply)
Q_DECLARE_METATYPE(HeterRunStatus)

inline constexpr quint64 kHeterMinHz = 51562500ull;
inline constexpr quint64 kHeterMaxHz = 6600000000ull;
inline constexpr int kHeterTempNa = -32768;
