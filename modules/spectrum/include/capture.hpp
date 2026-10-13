#pragma once

#include "app_state.hpp"
#include "circular_buffer.hpp"
#include "clip_monitor.hpp"
#include "rx_calibration.hpp"
#include "sample.hpp"
#include <chrono>
#include <complex>
#include <cstdint>
#include <vector>
#include <string>

class PlutoCaptureThread {
public:
    PlutoCaptureThread(
        CircularBuffer<CoherentSample>& fft_buf,
        CircularBuffer<CoherentSample>& df_buf,
        CircularBuffer<CoherentSample>& demod_buf,
        AppState& state);
    ~PlutoCaptureThread();

    void run();

private:
    bool open_device();
    bool configure_device();
    void close_device();

    // Усиление живёт отдельно от configure_device(): режим и уровень можно
    // переписать на работающем потоке, поэтому их применяет каждый проход цикла,
    // а не только переподключение. force=true — записать безусловно (после
    // connect, когда состояние чипа неизвестно), иначе только при изменении.
    void apply_gain(bool force);

    // Инверсия Rx2. Стоит до всей остальной настройки: применяется записью
    // initialize=1, а она перезапускает ad9361_setup() и сбрасывает чип к
    // platform data — полоса, частота дискретизации, усиление и LO, выставленные
    // раньше неё, после неё не выживют.
    void apply_phase_inversion();

    // Отключение адаптивных алгоритмов для стабильной фазы между каналами.
    // Убирает AM-to-PM эффект при изменении уровня сигнала.
    void configure_phase_stable();

    void poll_gain_readback();          // фактическое усиление и RSSI обратно в AppState
    static const char* gain_mode_attr(GainMode mode); // значение для gain_control_mode

    // Таблица читается один раз при старте потока и по запросу из UI, а
    // множитель пересчитывается раз в буфер: частота и усиление внутри буфера
    // постоянны, а тригонометрия на каждый отсчёт была бы чистой тратой.
    void reload_calibration();
    void update_calibration_correction();

    // Ручная подстройка фазы (manual_phase_trim_deg) — та же схема, что у
    // калибровки таблицы: пересчитывается раз в буфер, применяется отдельным
    // умножением, чтобы её можно было крутить независимо от галочки таблицы.
    void update_manual_trim();

    void process_rx_buffer();
    void log_error(const std::string& context, int error_code);
    void save_iq_file();

    // Прямой SPI-доступ к чипу (iio_device_reg_read), в отличие от именованных
    // debug-атрибутов вроде apply_phase_inversion() выше: адрес/значение, а не
    // название параметра. Диапазон 0x000..0x3FF — по верхнему определению в
    // ad9361_regs.h прошивки платы.
    void dump_ad9361_registers();

    CircularBuffer<CoherentSample>& fft_buf_;
    CircularBuffer<CoherentSample>& df_buf_ ;
    CircularBuffer<CoherentSample>& demod_buf_;
    AppState&                       state_  ;

    // Перегрузка входа: считается по тем же сырым отсчётам, что и так
    // разбираются в process_rx_buffer(), отдельного прохода по памяти нет.
    ClipMonitor         clip_monitor_;

    RxCalibration       calibration_;
    std::complex<float> cal_correction_ { 1.0f, 0.0f };
    bool                cal_active_     { false };

    std::complex<float> manual_trim_correction_ { 1.0f, 0.0f };
    bool                manual_trim_active_      { false };

    // Время истории фазы — своя переменная, а не clip_monitor_'а: та копится
    // окнами по ~100 мс и обнуляется по своей логике, а тут нужно ровно то,
    // что публикуется в phase_hist_* каждый process_rx_buffer().
    double phase_stream_time_s_ { 0.0 };

    struct iio_context* ctx_    { nullptr };
    struct iio_device*  rx_dev_ { nullptr };
    struct iio_device*  phy_dev_{ nullptr };
    
    struct iio_channel* rx_i0_  { nullptr };
    struct iio_channel* rx_q0_  { nullptr };
    struct iio_channel* rx_i1_  { nullptr };
    struct iio_channel* rx_q1_  { nullptr };

    // каналы управления AD9361 (voltage0/voltage1 устройства ad9361-phy) —
    // держим найденными, потому что усиление трогается в цикле захвата
    struct iio_channel* phy_chn0_{ nullptr };
    struct iio_channel* phy_chn1_{ nullptr };

    struct iio_buffer* rx_buf_  { nullptr };

    uint64_t current_freq_{ 0 };
    uint32_t current_sample_rate_{ 0 };
    PlutoChannelMode current_mode_{ PlutoChannelMode::Channel_1 };

    // последнее, что реально записано в чип, — чтобы не переписывать атрибуты
    // усиления каждый проход цикла (на сетевом libiio это лишние round trip'ы)
    GainMode current_gain_mode_{ GainMode::SlowAttack };
    int      current_gain_db_ { 0 };
    int      current_gain_db2_{ 0 };
    std::chrono::steady_clock::time_point last_gain_poll_{};
};