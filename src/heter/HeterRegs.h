#pragma once

#include <QString>
#include <QVector>

#include <cstdint>

/// Одно слово ADF4355-3 и подпись к нему.
struct HeterRegView {
    int index = 0;
    quint32 word = 0;
    QString title;
    QString meaning;
};

/// Образ регистров, который прошивка записывает в ADF4355-3 при заданных частоте и включениях.
/// По RS-485 микросхема регистры не отдаёт: это тот же расчёт, что в прошивке.
struct HeterRegImage {
    bool ok = false;
    QString error;
    int divider = 1;
    quint64 vcoHz = 0;
    int intN = 0;
    int frac1 = 0;
    int frac2 = 0;
    int mod2 = 0;
    double cpMa = 0.9;
    QVector<HeterRegView> regs;
};

/// refHz — 50 МГц, как в выпускаемой прошивке. powA/powB — коды 0…3 (−4/−1/+2/+5 дБм).
[[nodiscard]] HeterRegImage heterRegImage(quint64 freqHz, int cpCode, bool enA, int powA, bool enB, int powB,
                                          quint32 refHz = 50000000);
