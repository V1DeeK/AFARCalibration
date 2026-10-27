#pragma once

#include "Ppkh4Frame.h"

#include <string>

/// Состояние стенда «Настройки 1 канала»: файл .afarstate (JSON) и рядом с ним
/// состояние S2VNA .sta. Контракт: docs/contracts/data-formats.md, «Состояние стенда».
namespace afar::ppkh4 {

inline constexpr int kStateFormatVersion = 1;

/// Всё, что оператор настраивает на вкладке «Настройка 1 канала».
struct ChannelState {
    FrameFormat format;
    LineConfig lines;
    int phase_code = 0;
    int atten_code = 0;
    SweepMode sweep_mode = SweepMode::Phase;
    SweepRange sweep_phase;
    SweepRange sweep_atten;
    int sweep_dwell_ms = 1500;
};

struct BenchState {
    std::string name;
    std::string saved_at;         ///< ISO 8601, местное время
    /// Имя файла .sta рядом с .afarstate; пусто — состояние S2VNA не сохранялось.
    std::string s2vna_state_file;
    ChannelState channel;
};

[[nodiscard]] std::string to_json_text(const BenchState& state);
/// false и причина в error — файл не разобран или раскладка кадра недопустима.
[[nodiscard]] bool from_json_text(const std::string& text, BenchState& state, std::string* error = nullptr);

}  // namespace afar::ppkh4
