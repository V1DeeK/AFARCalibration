#include "ChannelAnalysis.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QMap>
#include <QTextStream>

#include <algorithm>
#include <cmath>

using afar::cal::ChipGrid;

namespace {

constexpr qint64 kMaxCsvBytes = 2LL * 1024 * 1024 * 1024;

QString num(double v, int digits = 3)
{
    return QString::number(v, 'f', digits);
}

double wrapDeg(double x)
{
    return x - 360.0 * std::floor((x + 180.0) / 360.0);
}

double mod360(double x)
{
    return x - 360.0 * std::floor(x / 360.0);
}

bool openForWrite(QFile& file, QString* error)
{
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        *error = QStringLiteral("не удалось записать %1: %2").arg(file.fileName(), file.errorString());
        return false;
    }
    return true;
}

}  // namespace

QString loadChannelGrids(const QString& csvPath, std::vector<ChipGrid>* grids, int* rejected)
{
    grids->clear();
    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringLiteral("не удалось открыть %1: %2").arg(csvPath, file.errorString());
    }
    if (file.size() > kMaxCsvBytes) {
        return QStringLiteral("файл больше 2 ГБ — это не measurements.csv прохода");
    }
    const QByteArray header = file.readLine().trimmed();
    QList<QByteArray> columns = header.split(',');
    if (!columns.isEmpty() && columns.first().startsWith("\xEF\xBB\xBF")) {
        columns.first().remove(0, 3);
    }
    const int cCh = static_cast<int>(columns.indexOf("channel_id"));
    const int cF = static_cast<int>(columns.indexOf("freq_hz"));
    const int cP = static_cast<int>(columns.indexOf("phase_code"));
    const int cA = static_cast<int>(columns.indexOf("att_code"));
    const int cG = static_cast<int>(columns.indexOf("gain_db"));
    const int cPhi = static_cast<int>(columns.indexOf("phase_deg"));
    if (cCh < 0 || cF < 0 || cP < 0 || cA < 0 || cG < 0) {
        return QStringLiteral("в CSV нет столбцов channel_id, freq_hz, phase_code, att_code, gain_db");
    }
    if (cPhi < 0) {
        // Номинальная фаза команды измеренную не заменяет (методика, раздел 1).
        return QStringLiteral("в CSV нет столбца phase_deg с измеренной фазой — таблицу фазовой коррекции построить нельзя");
    }
    const int need = std::max({cCh, cF, cP, cA, cG, cPhi}) + 1;
    QHash<QPair<int, qint64>, int> index;
    int bad = 0;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QList<QByteArray> cells = line.split(',');
        if (cells.size() < need) {
            ++bad;
            continue;
        }
        bool ok[6] = {};
        const int channel = cells.at(cCh).toInt(&ok[0]);
        const double freq = cells.at(cF).toDouble(&ok[1]);
        const int p = cells.at(cP).toInt(&ok[2]);
        const int a = cells.at(cA).toInt(&ok[3]);
        const double gain = cells.at(cG).toDouble(&ok[4]);
        const double phase = cells.at(cPhi).toDouble(&ok[5]);
        if (!std::all_of(std::begin(ok), std::end(ok), [](bool v) { return v; }) || p < 0 || p >= afar::cal::kChipCodes
            || a < 0 || a >= afar::cal::kChipCodes || channel < 0 || channel > 255 || !std::isfinite(freq) || freq <= 0
            || freq > 1e11 || !std::isfinite(gain) || !std::isfinite(phase)) {
            ++bad;
            continue;
        }
        const QPair<int, qint64> key(channel, std::llround(freq));
        auto it = index.find(key);
        if (it == index.end()) {
            if (grids->size() >= 100000) {
                return QStringLiteral("слишком много сочетаний канал × частота");
            }
            ChipGrid grid;
            grid.channel = channel;
            grid.freq_hz = static_cast<double>(key.second);
            grids->push_back(grid);
            it = index.insert(key, static_cast<int>(grids->size()) - 1);
        }
        ChipGrid& grid = (*grids)[static_cast<std::size_t>(it.value())];
        const auto s = static_cast<std::size_t>(afar::cal::chip_state(p, a));
        // Повтор той же точки: остаётся последнее измерение, как при дозаписи прохода.
        grid.gain_db[s] = gain;
        grid.phase_deg[s] = phase;
        grid.present[s] = true;
    }
    if (rejected != nullptr) {
        *rejected = bad;
    }
    if (grids->empty()) {
        return QStringLiteral("в CSV нет ни одной годной строки");
    }
    std::sort(grids->begin(), grids->end(), [](const ChipGrid& l, const ChipGrid& r) {
        return l.freq_hz != r.freq_hz ? l.freq_hz < r.freq_hz : l.channel < r.channel;
    });
    return {};
}

ChannelAnalysisResult analyzeChannelMeasurements(const QString& csvPath, const QString& folder,
                                                 const ChannelAnalysisOptions& options,
                                                 const std::function<void(int, int)>& progress,
                                                 const std::atomic<bool>* cancel)
{
    using namespace afar::cal;
    ChannelAnalysisResult result;
    result.folder = folder;
    std::vector<ChipGrid> grids;
    int rejected = 0;
    result.error = loadChannelGrids(csvPath, &grids, &rejected);
    if (!result.error.isEmpty()) {
        return result;
    }
    if (rejected > 0) {
        result.warnings << QStringLiteral("Отброшено строк CSV с ошибками: %1.").arg(rejected);
    }
    if (!QDir().mkpath(folder + QStringLiteral("/lut"))) {
        result.error = QStringLiteral("не удалось создать папку ") + folder;
        return result;
    }

    QMap<qint64, std::vector<const ChipGrid*>> byFreq;
    QMap<int, int> channelSet;
    for (const ChipGrid& g : grids) {
        byFreq[std::llround(g.freq_hz)].push_back(&g);
        ++channelSet[g.channel];
    }
    result.channels = static_cast<int>(channelSet.size());
    result.frequencies = static_cast<int>(byFreq.size());
    if (result.frequencies == 1) {
        result.warnings << QStringLiteral("Измерена одна частота %1 Гц. Для полосы 4,9–6,0 ГГц повторите проход "
                                          "с заданной полосой: таблицы коррекции действуют только на измеренных частотах.")
                               .arg(byFreq.firstKey());
    }

    QFile checkCsv(folder + QStringLiteral("/datasheet_check.csv"));
    QFile gainCsv(folder + QStringLiteral("/correction_gain.csv"));
    QFile phaseCsv(folder + QStringLiteral("/correction_phase.csv"));
    QFile summary(folder + QStringLiteral("/datasheet_check.txt"));
    if (!openForWrite(checkCsv, &result.error) || !openForWrite(gainCsv, &result.error)
        || !openForWrite(phaseCsv, &result.error) || !openForWrite(summary, &result.error)) {
        return result;
    }
    result.summaryPath = summary.fileName();
    checkCsv.write("channel_id,freq_hz,points,"
                   "hmc_state_violations,hmc_states_checked,hmc_max_excess_db,hmc_rms_state_error_db,"
                   "hmc_max_state_error_db,hmc_max_step_error_db,hmc_steps_over_typ,hmc_range_db,"
                   "hmc_relative_phase_mean_deg,hmc_relative_phase_max_deg,"
                   "maps_rms_phase_error_deg,maps_max_phase_error_deg,maps_rms_amp_error_db,maps_amp_pp_db,"
                   "maps_bit5p6_deg,maps_bit11p2_deg,maps_bit22p5_deg,maps_bit45_deg,maps_bit90_deg,"
                   "maps_bit180_deg,maps_sum_bits_deg,maps_monotonic\n");
    gainCsv.write("channel_id,freq_hz,att_code,nominal_atten_db,actual_atten_db,atten_error_db,"
                  "atten_min_db,atten_max_db,phase_shift_deg,limit_db,within_limit\n");
    phaseCsv.write("channel_id,freq_hz,phase_code,nominal_phase_deg,actual_phase_deg,phase_error_deg,"
                   "gain_delta_db\n");

    // Сводка по каналам: худшее по полосе.
    struct Worst {
        int freqs = 0, hmcFreqsOver = 0, violations = 0, checked = 0;
        double excess = 0, excessHz = 0, rmsAtten = 0, relPhaseMin = 1e9, relPhaseMax = 0;
        double rmsPhaseMin = 1e9, rmsPhaseMax = 0, rmsPhaseMaxHz = 0, rmsAmpMax = 0, ampPpMax = 0, ampPpHz = 0;
        std::array<double, 7> bitWorst{};
        std::array<double, 7> bitWorstShift{};
        bool monotonic = true;
        int incomplete = 0;
    };
    QMap<int, Worst> worst;

    const int total = result.frequencies;
    int done = 0;
    for (auto it = byFreq.cbegin(); it != byFreq.cend(); ++it) {
        if (cancel != nullptr && cancel->load()) {
            result.error = QStringLiteral("остановлено");
            return result;
        }
        const qint64 freq = it.key();
        bool allComplete = true;
        for (const ChipGrid* g : it.value()) {
            const AttenCheck a = check_attenuator(*g);
            const PhaseCheck p = check_phase_shifter(*g);
            Worst& w = worst[g->channel];
            ++w.freqs;
            if (!g->complete()) {
                ++w.incomplete;
                allComplete = false;
            }
            w.violations += a.state_violations;
            w.checked += a.states_checked;
            w.hmcFreqsOver += a.state_violations > 0;
            if (a.max_excess_db > w.excess) {
                w.excess = a.max_excess_db;
                w.excessHz = g->freq_hz;
            }
            w.rmsAtten = std::max(w.rmsAtten, a.rms_state_error_db);
            w.relPhaseMin = std::min(w.relPhaseMin, a.mean_relative_phase_deg);
            w.relPhaseMax = std::max(w.relPhaseMax, a.mean_relative_phase_deg);
            w.rmsPhaseMin = std::min(w.rmsPhaseMin, p.rms_phase_error_deg);
            if (p.rms_phase_error_deg > w.rmsPhaseMax) {
                w.rmsPhaseMax = p.rms_phase_error_deg;
                w.rmsPhaseMaxHz = g->freq_hz;
            }
            w.rmsAmpMax = std::max(w.rmsAmpMax, p.rms_amp_error_db);
            if (p.amp_peak_to_peak_db > w.ampPpMax) {
                w.ampPpMax = p.amp_peak_to_peak_db;
                w.ampPpHz = g->freq_hz;
            }
            for (std::size_t b = 0; b < 7; ++b) {
                if (std::abs(p.bit_error_deg[b]) > std::abs(w.bitWorst[b])) {
                    w.bitWorst[b] = p.bit_error_deg[b];
                    w.bitWorstShift[b] = p.bit_shift_deg[b];
                }
            }
            w.monotonic = w.monotonic && p.monotonic;

            QStringList row{QString::number(g->channel), QString::number(freq), QString::number(g->count()),
                            QString::number(a.state_violations), QString::number(a.states_checked),
                            num(a.max_excess_db), num(a.rms_state_error_db), num(a.max_abs_state_error_db),
                            num(a.max_abs_step_error_db), QString::number(a.step_over_typ), num(a.mean_range_db),
                            num(a.mean_relative_phase_deg), num(a.max_relative_phase_deg),
                            num(p.rms_phase_error_deg), num(p.max_abs_phase_error_deg), num(p.rms_amp_error_db),
                            num(p.amp_peak_to_peak_db)};
            for (std::size_t b = 0; b < 7; ++b) {
                row << num(p.bit_shift_deg[b]);
            }
            row << (p.monotonic ? QStringLiteral("1") : QStringLiteral("0"));
            checkCsv.write(row.join(QLatin1Char(',')).toUtf8() + '\n');

            // Таблица коррекции усиления: фактическое ослабление кода, среднее по 64 кодам фазы.
            for (int a_code = 0; a_code < kChipCodes; ++a_code) {
                double sum = 0, lo = 1e9, hi = -1e9, sx = 0, sy = 0;
                int n = 0;
                for (int pc = 0; pc < kChipCodes; ++pc) {
                    const auto s = static_cast<std::size_t>(chip_state(pc, a_code));
                    const auto ref = static_cast<std::size_t>(chip_state(pc, kAttenZeroCode));
                    if (!g->present[s] || !g->present[ref]) {
                        continue;
                    }
                    const double att = g->gain_db[ref] - g->gain_db[s];
                    const double d = (g->phase_deg[s] - g->phase_deg[ref]) * M_PI / 180.0;
                    sum += att;
                    lo = std::min(lo, att);
                    hi = std::max(hi, att);
                    sx += std::cos(d);  // круговое среднее: обычное врёт около ±180°
                    sy += std::sin(d);
                    ++n;
                }
                if (n == 0) {
                    continue;
                }
                const double nominal = nominal_atten_db(a_code);
                const double actual = sum / n;
                const double limit = hmc624a_state_limit_db(nominal, g->freq_hz);
                gainCsv.write(QStringList{QString::number(g->channel), QString::number(freq),
                                          QString::number(a_code), num(nominal, 1), num(actual), num(actual - nominal),
                                          num(lo), num(hi), num(std::atan2(sy, sx) * 180.0 / M_PI), num(limit),
                                          std::abs(actual - nominal) <= limit ? QStringLiteral("1") : QStringLiteral("0")}
                                  .join(QLatin1Char(','))
                                  .toUtf8()
                              + '\n');
            }
            // Таблица коррекции фазы: фактический сдвиг кода при нулевом заданном ослаблении.
            const auto ref0 = static_cast<std::size_t>(chip_state(0, kAttenZeroCode));
            for (int pc = 0; pc < kChipCodes && g->present[ref0]; ++pc) {
                const auto s = static_cast<std::size_t>(chip_state(pc, kAttenZeroCode));
                if (!g->present[s]) {
                    continue;
                }
                const double nominal = kPhaseLsbDeg * pc;
                const double actual = mod360(g->phase_deg[ref0] - g->phase_deg[s]);
                phaseCsv.write(QStringList{QString::number(g->channel), QString::number(freq), QString::number(pc),
                                           num(nominal), num(actual), num(wrapDeg(actual - nominal)),
                                           num(g->gain_db[s] - g->gain_db[ref0])}
                                   .join(QLatin1Char(','))
                                   .toUtf8()
                               + '\n');
            }
        }

        // Совместная LUT (усиление + фаза) — только по полным сеткам всех каналов этой частоты.
        if (allComplete) {
            const FrequencyLut lut = build_frequency_lut(it.value(), options.lut);
            if (!lut.error.empty()) {
                result.warnings << QStringLiteral("%1 Гц: LUT не построена — %2").arg(freq).arg(QString::fromStdString(lut.error));
            } else {
                QFile out(folder + QStringLiteral("/lut/lut_%1.csv").arg(freq));
                if (!openForWrite(out, &result.error)) {
                    return result;
                }
                out.write("channel_number,freq_hz,gain_index,phase_index,target_gain_db,target_phase_offset_deg,"
                          "target_phase_deg,phase_code,att_code,predicted_gain_db,predicted_phase_deg,gain_error_db,"
                          "phase_error_deg,relative_complex_error,status\n");
                const int nq = static_cast<int>(lut.target_phases_deg.size());
                QByteArray block;
                for (std::size_t n = 0; n < lut.entries.size(); ++n) {
                    for (std::size_t k = 0; k < lut.target_gains_db.size(); ++k) {
                        for (int q = 0; q < nq; ++q) {
                            const LutEntry& e = lut.entries[n][k * static_cast<std::size_t>(nq) + static_cast<std::size_t>(q)];
                            block += QByteArray::number(lut.channels[n]) + ',' + QByteArray::number(freq) + ','
                                     + QByteArray::number(static_cast<int>(k)) + ',' + QByteArray::number(q) + ','
                                     + QByteArray::number(lut.target_gains_db[k], 'f', 3) + ','
                                     + QByteArray::number(360.0 * q / nq, 'f', 3) + ','
                                     + QByteArray::number(lut.target_phases_deg[static_cast<std::size_t>(q)], 'f', 3) + ','
                                     + QByteArray::number(e.phase_code) + ',' + QByteArray::number(e.att_code) + ','
                                     + QByteArray::number(e.predicted_gain_db, 'f', 3) + ','
                                     + QByteArray::number(e.predicted_phase_deg, 'f', 3) + ','
                                     + QByteArray::number(e.gain_error_db, 'f', 3) + ','
                                     + QByteArray::number(e.phase_error_deg, 'f', 3) + ','
                                     + QByteArray::number(e.relative_complex_error, 'f', 5) + ','
                                     + QByteArray::number(static_cast<int>(e.status)) + '\n';
                        }
                    }
                }
                if (out.write(block) != block.size()) {
                    result.error = QStringLiteral("не удалось записать ") + out.fileName();
                    return result;
                }
                ++result.lutFrequencies;
            }
        }
        if (progress) {
            progress(++done, total);
        }
    }

    QTextStream t(&summary);
    t.setEncoding(QStringConverter::Utf8);
    t << QStringLiteral("ПРОВЕРКА ПАРАМЕТРОВ ДАТАШИТОВ ПО ИЗМЕРЕНИЯМ ТРАКТА\n");
    t << QStringLiteral("Источник: ") << csvPath << '\n';
    t << QStringLiteral("Каналов: %1. Частот: %2 (%3 … %4 МГц). LUT построена для %5 частот.\n")
             .arg(result.channels).arg(result.frequencies)
             .arg(byFreq.firstKey() / 1e6, 0, 'f', 3).arg(byFreq.lastKey() / 1e6, 0, 'f', 3).arg(result.lutFrequencies);
    t << QStringLiteral("\nИзмерен весь тракт канала, а не отдельная микросхема: отклонение от даташита не доказывает "
                        "неисправность микросхемы. «Typ» — типовое значение, не гарантированный предел.\n");
    for (const QString& w : result.warnings) {
        t << QStringLiteral("ВНИМАНИЕ: ") << w << '\n';
    }
    static const QStringList bitNames{QStringLiteral("5,6°"), QStringLiteral("11,2°"), QStringLiteral("22,5°"),
                                      QStringLiteral("45°"), QStringLiteral("90°"), QStringLiteral("180°"),
                                      QStringLiteral("сумма")};
    for (auto it = worst.cbegin(); it != worst.cend(); ++it) {
        const Worst& w = it.value();
        t << QStringLiteral("\n=== Канал %1 (channel_id %2), частот: %3 ===\n").arg(it.key() + 1).arg(it.key()).arg(w.freqs);
        if (w.incomplete > 0) {
            t << QStringLiteral("  Неполная сетка 64×64 на %1 частотах — по ним LUT не строится.\n").arg(w.incomplete);
        }
        t << QStringLiteral("  HMC624A (Rev. B, табл. 1)\n");
        t << QStringLiteral("    Ошибка состояния, предел ±(0,3 + 3 %·A) дБ: %1 — вне предела %2 из %3 точек, на %4 из %5 частот")
                 .arg(w.violations == 0 ? QStringLiteral("ПОДТВЕРЖДЕНО") : QStringLiteral("НЕ ПОДТВЕРЖДЕНО"))
                 .arg(w.violations).arg(w.checked).arg(w.hmcFreqsOver).arg(w.freqs);
        if (w.violations > 0) {
            t << QStringLiteral("; наибольшее превышение %1 дБ на %2 МГц").arg(w.excess, 0, 'f', 2).arg(w.excessHz / 1e6, 0, 'f', 1);
        }
        t << QStringLiteral("\n    RMS ошибки ослабления, худшая по полосе: %1 дБ\n").arg(w.rmsAtten, 0, 'f', 2);
        t << QStringLiteral("    Сдвиг фазы 0 → 31,5 дБ (Typ 50° в 3–6 ГГц): %1…%2° по полосе\n")
                 .arg(w.relPhaseMin, 0, 'f', 1).arg(w.relPhaseMax, 0, 'f', 1);
        t << QStringLiteral("  MAPS-010165 (Rev. V3, стр. 2), при нулевом заданном ослаблении\n");
        t << QStringLiteral("    RMS ошибки фазы (Typ 4°): %1…%2° по полосе, худшая на %3 МГц — %4\n")
                 .arg(w.rmsPhaseMin, 0, 'f', 2).arg(w.rmsPhaseMax, 0, 'f', 2).arg(w.rmsPhaseMaxHz / 1e6, 0, 'f', 1)
                 .arg(w.rmsPhaseMax <= kMapsRmsPhaseTypDeg ? QStringLiteral("в пределах типового")
                                                           : QStringLiteral("выше типового"));
        t << QStringLiteral("    RMS ошибки амплитуды (Typ 0,4 дБ): до %1 дБ — %2\n")
                 .arg(w.rmsAmpMax, 0, 'f', 2)
                 .arg(w.rmsAmpMax <= kMapsRmsAmpTypDb ? QStringLiteral("в пределах типового") : QStringLiteral("выше типового"));
        t << QStringLiteral("    Размах усиления по 64 кодам фазы (Typ ±0,8 дБ → 1,6 дБ): до %1 дБ на %2 МГц — %3\n")
                 .arg(w.ampPpMax, 0, 'f', 2).arg(w.ampPpHz / 1e6, 0, 'f', 1)
                 .arg(w.ampPpMax <= 2 * kMapsAmpVariationTypDb ? QStringLiteral("в пределах типового")
                                                               : QStringLiteral("выше типового"));
        t << QStringLiteral("    Точность битов (наибольшее отклонение по полосе / Typ):\n");
        for (int b = 0; b < 7; ++b) {
            const auto i = static_cast<std::size_t>(b);
            t << QStringLiteral("      %1: измерено %2°, ошибка %3° / ±%4° — %5\n")
                     .arg(bitNames.at(b), -6).arg(w.bitWorstShift[i], 0, 'f', 2).arg(w.bitWorst[i], 0, 'f', 2)
                     .arg(kPhaseBitTypDeg[i], 0, 'f', 1)
                     .arg(std::abs(w.bitWorst[i]) <= kPhaseBitTypDeg[i] ? QStringLiteral("в пределах типового")
                                                                        : QStringLiteral("выше типового"));
        }
        t << QStringLiteral("    Монотонность фазы по коду: %1\n").arg(w.monotonic ? QStringLiteral("да") : QStringLiteral("нет"));
    }
    t << QStringLiteral("\nФайлы:\n"
                        "  datasheet_check.csv  — все показатели по каждому каналу и частоте\n"
                        "  correction_gain.csv  — таблица коррекции усиления: фактическое ослабление каждого кода аттенюатора\n"
                        "  correction_phase.csv — таблица коррекции фазы: фактический сдвиг каждого кода фазовращателя\n"
                        "  lut/lut_<Гц>.csv     — совместная LUT [канал][уровень][фаза] → (phase_code, att_code)\n"
                        "                         по методике calibration_16_channels_RU (критерий |H/Ht − 1|²)\n"
                        "LUT — прогноз по калибровочным данным: после записи кодов нужен независимый повторный замер.\n");
    t.flush();
    if (checkCsv.error() != QFile::NoError || gainCsv.error() != QFile::NoError || phaseCsv.error() != QFile::NoError
        || summary.error() != QFile::NoError) {
        result.error = QStringLiteral("ошибка записи файлов в ") + folder;
    }
    return result;
}
