#pragma once

#include <chrono>
#include <fstream>
#include <memory>
#include <string>

#include "app_state.hpp"
#include "circular_buffer.hpp"
#include "network_stats.hpp"
#include "peleng.hpp"
#include "rx_calibration.hpp"
#include "sample.hpp"

// Publishes per-subband monopulse results into AppState for the UI to plot.
// Two interchangeable sources of the same SubbandStats block:
//   local   — run the PDF's PL model (SubbandAccumulator) over the RX1/RX2 IQ stream
//   network — take blocks the Zynq PL already accumulated, over TCP
// Either way the PS half (SubbandEstimator) and the display are identical.
class DirectionFindingThread {
public:
    DirectionFindingThread(CircularBuffer<CoherentSample>& df_buf, AppState& state);
    void run();

private:
    void run_local_step();
    void run_network_step();
    void estimate_and_publish(const std::vector<SubbandStats>& stats,
                               int accum_frames, int subband_size, int fft_size,
                               double sample_rate_hz, double center_hz,
                               const std::vector<Complex>& c2_per_subband,
                               bool is_display_source, int source_id);
    void publish_results(const std::vector<SubbandResult>& results,
                          int accum_frames, int subband_size, int fft_size, double center_hz);

    // Пересоздаёт локальный accumulator_ под заданную геометрию, если она
    // отличается от текущей — used to align the local (PC) FFT/subband/accum
    // geometry to whatever the board is currently using while comparing, so
    // subband N means the same thing on both sides. No-op (cheap) otherwise.
    void ensure_local_geometry(int fft_size, int subband_size, int accum_frames);

    // Пересчитывает C1[p]/C2[p] под текущие частоту, усиление и геометрию БПФ.
    // Дёшево, когда ничего не изменилось: вызов сравнивает ключ и выходит.
    void update_calibration(double center_hz, double sample_rate_hz,
                             int fft_size, int subband_size);

    // Журнал в CSV для последующей обработки на питоне (python/peleng_analysis.py).
    // Пишется здесь же, где считаются результаты, — так в файл попадает каждый
    // накопленный блок, а не тот один, что успел увидеть кадр отрисовки.
    // Вызывается для КАЖДОГО посчитанного блока, независимо от того, какой
    // источник сейчас выбран для отображения — в режиме сравнения оба источника
    // должны попасть в лог.
    void log_block(int source_id, const std::vector<SubbandResult>& results,
                    int num_subbands, double sample_rate_hz, double center_hz);
    void handle_log_start(int num_subbands, double sample_rate_hz, double center_hz);
    bool open_log(const std::string& path, int num_subbands, double sample_rate_hz, double center_hz);
    void close_log();
    void write_log_block(int source_id, const std::vector<SubbandResult>& results, double center_hz);

    CircularBuffer<CoherentSample>& df_buf_;
    AppState& state_;

    std::ofstream                         log_file_;
    std::chrono::steady_clock::time_point log_started_at_;
    uint64_t                              log_block_index_{ 0 };
    std::string                           log_row_buf_; // переиспользуется между блоками, чтобы не выделять память на каждый

    std::unique_ptr<SubbandAccumulator>     accumulator_;
    std::unique_ptr<SubbandEstimator>       estimator_;
    std::unique_ptr<NetworkSubbandReceiver> receiver_; // created on first switch to network mode

    std::vector<CoherentSample> batch_;
    std::vector<Complex>        rx1_;
    std::vector<Complex>        rx2_;
    std::vector<SubbandStats>   net_stats_;

    // Геометрия последнего блока, полученного с платы — источник истины для
    // ensure_local_geometry() в режиме сравнения. Живёт независимо от того,
    // включено ли сравнение сейчас: обновляется всегда, когда пришёл сетевой
    // блок, чтобы при включении сравнения не ждать нового блока заново.
    bool have_board_geometry_{ false };
    int  board_fft_{ 0 };
    int  board_subband_{ 0 };
    int  board_accum_{ 0 };

    // Своя копия таблицы: поток захвата держит собственную и применяет из неё
    // только константу, к отсчётам. Здесь нужна частотная часть — та, которую
    // одним комплексным множителем на отсчёт не применить.
    RxCalibration        cal_;
    uint32_t             cal_generation_{ 0 };
    bool                 cal_load_tried_{ false };
    std::vector<Complex> c2_bins_;      // локальный путь: только задержка
    std::vector<Complex> c2_subbands_;  // сетевой путь: вся поправка целиком

    // Ключ, при неизменности которого пересчитывать нечего.
    bool   cal_key_enabled_{ false };
    double cal_key_freq_{ 0.0 };
    double cal_key_rate_{ 0.0 };
    int    cal_key_gain_{ -1000 };
    int    cal_key_fft_{ 0 };
    int    cal_key_sub_{ 0 };
    uint32_t cal_key_gen_{ 0 };
};
