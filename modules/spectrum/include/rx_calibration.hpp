#pragma once

#include <complex>
#include <string>
#include <vector>

// Таблица собственного перекоса приёмников AD9361 (Rx2 относительно Rx1),
// снятая генератором по сетке частота × усиление и записанная
// python/make_rx_calibration.py в data/rx_calibration.csv.
//
// Таблица содержит именно перекос чипа: она построена как полусумма двух
// прогонов с переставленными кабелями, поэтому тракт стенда из неё вычтен.
// Знак — arg(Rx2/Rx1); поправка, которую надо применить к Rx2, ему обратна,
// и её отдаёт correction(). Условия применимости (инверсия Rx2, порт, ручное
// усиление) записаны в шапке самого файла.
//
// Начиная с формата v2 в таблице есть ещё колонка delay_ns — разность групповых
// задержек Rx2 и Rx1. Внутриполосный перекос этих приёмников измерен и оказался
// именно задержкой: фаза растёт линейно со смещением от гетеродина, а амплитуда
// в пределах шума плоская. Поэтому вместо кривой на каждую подполосу хватает
// одного числа на строку, а перекос на бине k считается как
//     phase_deg + 360 * f_off[k] * delay_ns * 1e-9.
// Применять задержку во временной области одним комплексным множителем нельзя —
// это фильтр, а не поворот; для неё есть correction(freq, gain, offset).
// Файлы v1 читаются по-прежнему, задержка в них считается нулевой.
class RxCalibration {
public:
    // Читает CSV. При ошибке возвращает false и пишет причину в error;
    // прежняя таблица при этом не портится — либо новая целиком, либо старая.
    bool load(const std::string& path, std::string& error);

    bool valid() const { return !gains_.empty() && !freqs_.empty(); }
    const std::string& source() const { return source_; }

    // Перекос в точке (билинейная интерполяция, за границами сетки — край).
    // Возвращает false, если таблица не загружена.
    bool imbalance(double freq_hz, double gain_db, float& phase_deg, float& amp_db) const;

    // Есть ли в загруженной таблице колонка задержки (то есть это v2).
    bool has_delay() const { return has_delay_; }

    // Разность групповых задержек Rx2-Rx1, нс. Без колонки — 0, и тогда
    // correction() со смещением совпадает с correction() без него.
    float delay_ns(double freq_hz, double gain_db) const;

    // Точка внутри снятой сетки? Снаружи imbalance() возвращает край, и это
    // уже экстраполяция — вызывающий должен иметь возможность это показать.
    bool in_range(double freq_hz, double gain_db) const;

    // Множитель для Rx2, снимающий перекос: 10^(-amp/20) * exp(-i*phase).
    // Без таблицы — единица, чтобы вызывающему не приходилось ветвиться.
    std::complex<float> correction(double freq_hz, double gain_db) const;

    // То же для одного бина спектра: offset_hz — смещение бина от гетеродина.
    // Это и есть C2[p]/C1[p] из раздела 19.4 PDF. Вызывать там, где спектр уже
    // посчитан (SubbandAccumulator, PSD), а не на отсчётах.
    std::complex<float> correction(double freq_hz, double gain_db, double offset_hz) const;

private:
    // Билинейная интерполяция одной из плоскостей таблицы в точке (freq, gain).
    double interpolate(const std::vector<float>& values, double freq_hz, double gain_db) const;

    // Сетка хранится по осям, значения — плотной матрицей freq × gain: файл
    // пишется полным прямоугольником, и так интерполяция не ищет соседей поиском.
    std::vector<double> freqs_;     // Гц, по возрастанию
    std::vector<double> gains_;     // дБ, по возрастанию
    std::vector<float>  phase_;     // градусы, [freq * gains_.size() + gain]
    std::vector<float>  amp_;       // дБ
    std::vector<float>  delay_;     // нс, та же раскладка; нули для файлов v1
    bool                has_delay_ = false;
    std::string         source_;
};
