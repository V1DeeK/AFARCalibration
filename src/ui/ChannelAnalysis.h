#pragma once

#include "ChipCheck.h"

#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <vector>

/// Разбор measurements.csv прохода 64×64 (channel_id,freq_hz,phase_code,att_code,gain_db,phase_deg):
/// проверка параметров даташитов HMC624A и MAPS-010165 и таблицы коррекции усиления и фазы
/// по каждой измеренной частоте и каждому каналу.
struct ChannelAnalysisResult {
    QString error;        ///< пусто — успех
    QString folder;       ///< куда записаны файлы
    QString summaryPath;  ///< datasheet_check.txt
    int channels = 0;
    int frequencies = 0;
    int lutFrequencies = 0;
    QStringList warnings;
};

struct ChannelAnalysisOptions {
    afar::cal::LutOptions lut;
};

/// Читает CSV. Неполные сетки остаются с пропусками: нулём не заполняются.
/// Строки вне 0…63 и неконечные числа отбрасываются и считаются в rejected.
[[nodiscard]] QString loadChannelGrids(const QString& csvPath, std::vector<afar::cal::ChipGrid>* grids,
                                       int* rejected = nullptr);

/// Пишет в folder: datasheet_check.txt, datasheet_check.csv, correction_gain.csv,
/// correction_phase.csv и lut/lut_<частота>.csv. progress(сделано, всего) — из рабочего потока.
[[nodiscard]] ChannelAnalysisResult analyzeChannelMeasurements(
    const QString& csvPath, const QString& folder, const ChannelAnalysisOptions& options,
    const std::function<void(int, int)>& progress = {}, const std::atomic<bool>* cancel = nullptr);
