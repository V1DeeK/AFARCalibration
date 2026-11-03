#include "HeterRegs.h"

namespace {

quint32 gcd32(quint32 a, quint32 b)
{
    while (b != 0) {
        const quint32 t = a % b;
        a = b;
        b = t;
    }
    return a;
}

}  // namespace

HeterRegImage heterRegImage(quint64 freqHz, int cpCode, bool enA, int powA, bool enB, int powB, quint32 refHz)
{
    HeterRegImage image;
    const int cp = qBound(0, cpCode, 15);
    image.cpMa = (cp + 1) * 0.3;
    if (refHz < 10000000u || refHz > 75000000u) {
        image.error = QStringLiteral("опора вне 10…75 МГц, расчёт регистров прошивки для неё не сделан");
        return image;
    }
    if (freqHz < 51562500ull || freqHz > 6600000000ull) {
        image.error = QStringLiteral("частота вне 51,5625…6600 МГц");
        return image;
    }

    int divSel = 0;
    quint64 vco = freqHz;
    while (vco < 3300000000ull && divSel < 6) {
        vco <<= 1;
        ++divSel;
    }
    if (vco < 3300000000ull || vco > 6600000000ull) {
        image.error = QStringLiteral("ГУН не попадает в 3,3…6,6 ГГц");
        return image;
    }

    const quint32 pfd = refHz;
    quint32 intN = static_cast<quint32>(vco / pfd);
    const quint64 rem = vco % pfd;
    quint32 frac1 = static_cast<quint32>((rem << 24) / pfd);
    const quint64 rem2 = (rem << 24) % pfd;
    const quint32 g = gcd32(static_cast<quint32>(rem2), pfd);
    quint32 mod2 = pfd / g;
    quint32 frac2 = static_cast<quint32>(rem2 / g);
    if (frac2 == 0) {
        mod2 = 2;
    } else if (mod2 > 16383u) {
        mod2 = 16383u;
        frac2 = static_cast<quint32>((rem2 * mod2 + pfd / 2u) / pfd);
        if (frac2 == mod2) {
            frac2 = 0;
            mod2 = 2;
            if (++frac1 == (1u << 24)) {
                frac1 = 0;
                ++intN;
            }
        }
    }
    const bool prescaler89 = intN >= 75u;
    if ((!prescaler89 && (intN < 23u || intN > 32767u)) || intN > 65535u) {
        image.error = QStringLiteral("целое N вне допустимого диапазона");
        return image;
    }

    const quint32 vb = pfd / 2400000u + 1u;
    const quint32 adcDiv = (pfd - 200000u + 399999u) / 400000u;
    const quint32 timeout = (pfd + 999999u) / 1000000u;
    quint32 bleed = 0;
    quint32 neg = 0;
    if ((frac1 | frac2) != 0) {
        bleed = static_cast<quint32>((39ull * pfd * static_cast<quint32>(cp + 1) * 300u) / (61440000ull * 900u));
        if (bleed > 255u) {
            bleed = 255u;
        }
        neg = 1u << 29;
    }
    const quint32 outputBits = (static_cast<quint32>(enB) << 9) | (static_cast<quint32>(powB & 3) << 7)
                               | (static_cast<quint32>(enA) << 6) | (static_cast<quint32>(powA & 3) << 4);
    const quint32 r6 = neg | 0x14000000u | (1u << 24) | (static_cast<quint32>(divSel) << 21) | (bleed << 13) | outputBits | 6u;
    const quint32 r4 = (6u << 27) | (1u << 15) | (1u << 14) | (static_cast<quint32>(cp & 15) << 10) | (1u << 8) | (1u << 7) | 4u;

    const quint32 words[13] = {
        (1u << 21) | (prescaler89 ? (1u << 20) : 0u) | (intN << 4),
        (frac1 << 4) | 1u,
        (frac2 << 18) | (mod2 << 4) | 2u,
        3u,
        r4,
        0x00800025u,
        r6,
        0x12000267u | (((frac1 | frac2) == 0u) ? (1u << 4) : 0u),
        0x1A69A6B8u,
        (vb << 24) | (timeout << 14) | (31u << 9) | (21u << 4) | 9u,
        0x60000000u | (adcDiv << 6) | 0x3Au,
        0x0081200Bu,
        0x0001050Cu,
    };
    const QString titles[13] = {
        QStringLiteral("R0 · целое N, автокалибровка"),
        QStringLiteral("R1 · дробная часть FRAC1"),
        QStringLiteral("R2 · FRAC2 и MOD2"),
        QStringLiteral("R3 · сигма-дельта"),
        QStringLiteral("R4 · опора, ток накачки, MUXOUT"),
        QStringLiteral("R5 · фиксированное слово"),
        QStringLiteral("R6 · делитель, выходы, ток утечки"),
        QStringLiteral("R7 · детектор захвата"),
        QStringLiteral("R8 · фиксированное слово"),
        QStringLiteral("R9 · выбор полосы ГУН"),
        QStringLiteral("R10 · АЦП калибровки"),
        QStringLiteral("R11 · фиксированное слово"),
        QStringLiteral("R12 · resync"),
    };
    const QString meanings[13] = {
        QStringLiteral("INT %1, прескалер %2, автокалибровка включена. Это целое число деления ГУН на 50 МГц.")
            .arg(intN)
            .arg(prescaler89 ? QStringLiteral("8/9") : QStringLiteral("4/5")),
        QStringLiteral("FRAC1 %1 — старшая дробная часть. Вместе с INT задаёт частоту ГУН.").arg(frac1),
        QStringLiteral("FRAC2 %1, MOD2 %2 — младшая дробь. MOD2 = 1 прошивка не пишет.").arg(frac2).arg(mod2),
        QStringLiteral("Сброс сигма-дельта по записи R0. Подстройка фазы и resync выключены."),
        QStringLiteral("Делитель опоры R = 1, удвоитель выключен. Ток накачки — код %1, номинально %2 мА. "
                       "MUXOUT — цифровой захват ФАПЧ на PB1, логика 3,3 В.")
            .arg(cp)
            .arg(image.cpMa, 0, 'f', 1),
        QStringLiteral("Постоянное слово ADF4355-3. Бит DB5 = 1, как в карте регистров и программе ADI."),
        QStringLiteral("Делитель выхода /%1, обратная связь с ГУН. A %2, B %3. Ток утечки дробного режима %4.")
            .arg(1 << divSel)
            .arg(enA ? QStringLiteral("включён") : QStringLiteral("выключен"))
            .arg(enB ? QStringLiteral("включён") : QStringLiteral("выключен"))
            .arg((frac1 | frac2) == 0 ? QStringLiteral("выключен: целое N") : QStringLiteral("включён, код %1").arg(bleed)),
        QStringLiteral("LE sync, 4096 циклов до объявления захвата. Окно детектора %1.")
            .arg((frac1 | frac2) == 0 ? QStringLiteral("2,9 нс, целое N") : QStringLiteral("12 нс, дробное N")),
        QStringLiteral("Постоянное слово ADF4355-3, его прошивка не пересчитывает."),
        QStringLiteral("Делитель полосы ГУН %1, тайм-аут калибровки %2. Резерв DB13:9 = 11111, это не ALC.")
            .arg(vb)
            .arg(timeout),
        QStringLiteral("Делитель такта АЦП %1, преобразование включено. Основа слова 0x60000000 — для ADF4355-3.")
            .arg(adcDiv),
        QStringLiteral("Постоянное слово ADF4355-3."),
        QStringLiteral("Делитель resync = 1, зарезервированные биты DB10 и DB8 установлены."),
    };

    image.ok = true;
    image.divider = 1 << divSel;
    image.vcoHz = vco;
    image.intN = static_cast<int>(intN);
    image.frac1 = static_cast<int>(frac1);
    image.frac2 = static_cast<int>(frac2);
    image.mod2 = static_cast<int>(mod2);
    image.regs.reserve(13);
    for (int i = 0; i < 13; ++i) {
        HeterRegView view;
        view.index = i;
        view.word = words[i];
        view.title = titles[i];
        view.meaning = meanings[i];
        image.regs.append(view);
    }
    return image;
}
