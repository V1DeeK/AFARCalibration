#pragma once

#include <string>
#include <utility>
#include <vector>

/// Кадр фазы и аттенюации одного канала для платы ППКХ4 (разъём X2).
/// Контракт: docs/contracts/ppkh4-usb-cdc.md. Без Qt: используется и GUI, и тестами.
namespace afar::ppkh4 {

/// Прошивка ППКХ4 принимает кадр до 256 бит (MAX_BITS в firmware/src/main.c).
inline constexpr int kMaxFrameBits = 256;
inline constexpr int kMaxFieldBits = 16;

/// Поле кода внутри кадра: младший бит, ширина, шаг и способ укладки.
struct FieldFormat {
    int offset = 0;       ///< номер младшего бита поля в кадре
    int width = 6;        ///< ширина поля, бит
    double lsb = 1.0;     ///< шаг кода: градусы (фаза) или дБ (аттенюация)
    bool invert = false;  ///< инвертировать биты поля
    bool reverse = false; ///< обратный порядок бит внутри поля
};

/// Раскладка кадра. По умолчанию — 12 бит: 11…6 аттенюация, 5…0 фаза.
struct FrameFormat {
    int total_bits = 12;
    std::string fixed_hex = "0";  ///< биты, которые не заняты полями
    FieldFormat phase{0, 6, 5.625, false, false};
    FieldFormat atten{6, 6, 0.5, false, false};
};

/// Временная диаграмма линий DATA/CLK/LE (команды CFG прошивки).
struct LineConfig {
    int lsb_first = 0;  ///< 0 — старшим битом вперёд
    int clk_idle = 0;   ///< 0 — защёлка по фронту, 1 — по спаду
    int le_active = 1;
    int le_mode = 0;    ///< 0 — импульс после данных, 1 — активен во время передачи
    int half_us = 1;    ///< полупериод CLK, мкс (1…10000)
    int le_us = 1;      ///< импульс LE, мкс (1…10000)
};

[[nodiscard]] int max_code(int width);

/// Фаза: код × шаг, от 0°.
[[nodiscard]] double phase_deg(int code, const FieldFormat& field);
/// Аттенюация: код 0 (все нули) = −31.5 дБ, максимальный код (все единицы) = 0 дБ.
[[nodiscard]] double atten_db(int code, const FieldFormat& field);
[[nodiscard]] int phase_code_from_deg(double deg, const FieldFormat& field);
/// Знак значения не учитывается: 5 и −5 дБ дают один и тот же код.
[[nodiscard]] int atten_code_from_db(double db, const FieldFormat& field);
[[nodiscard]] int clamp_code(int code, const FieldFormat& field);

/// Биты кадра, индекс = номер бита (0 — младший).
[[nodiscard]] std::vector<bool> build_frame(const FrameFormat& format, int phase_code, int atten_code);
/// Шестнадцатеричная запись кадра: заглавные буквы, ровно ceil(total/4) цифр.
[[nodiscard]] std::string frame_hex(const std::vector<bool>& bits);

/// Поля фазы и аттенюации перекрываются.
[[nodiscard]] bool fields_overlap(const FrameFormat& format);
/// Хотя бы одно поле выходит за пределы кадра.
[[nodiscard]] bool fields_outside(const FrameFormat& format);
/// Раскладку можно отправлять в плату: размеры в пределах прошивки.
[[nodiscard]] bool format_valid(const FrameFormat& format);

/// Что перебирать на вкладке: одно поле, все сочетания или оба поля шагом вместе.
enum class SweepMode {
    Phase,     ///< только фаза, аттенюация не меняется
    Atten,     ///< только аттенюация, фаза не меняется
    Grid,      ///< все сочетания: аттенюация — внешний цикл, фаза — внутренний
    Together,  ///< оба кода шагают одновременно, пока не кончится более короткий диапазон
};

/// Диапазон кодов «от»…«до» с шагом; «от» > «до» — перебор вниз.
struct SweepRange {
    int from = 0;
    int to = 63;
    int step = 1;
};

/// Пары (фаза, аттенюация) одного прохода перебора. Коды обрезаются по ширине поля,
/// неперебираемое поле остаётся равным текущему коду.
[[nodiscard]] std::vector<std::pair<int, int>> sweep_points(SweepMode mode, const SweepRange& phase,
                                                            const SweepRange& atten, int current_phase,
                                                            int current_atten, const FrameFormat& format);

/// «SEND <бит> <hex>».
[[nodiscard]] std::string send_command(const FrameFormat& format, int phase_code, int atten_code);
/// Команды CFG в том порядке, в каком их шлёт исходная страница 1Chanel.
[[nodiscard]] std::vector<std::string> cfg_commands(const LineConfig& lines);

}  // namespace afar::ppkh4
