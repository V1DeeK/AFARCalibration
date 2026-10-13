#pragma once

// Wideband monopulse direction-finding pipeline — software model of the PL/PS
// split described in ad9361_zynq7020_wideband_pl_algorithm.pdf:
//   PL (SubbandAccumulator): DC removal, window, FFT per channel, Sigma/Delta,
//                             per-bin cross statistics, accumulation into
//                             frequency subbands over M frames, ping-pong buffer.
//   PS (SubbandEstimator):   rho[p] = Delta/Sigma, coherence gamma^2[p], SNR[p].
//
// First version: fixed boresight beam (alpha = 1, no angle scanning).

#include <complex>
#include <cstdint>
#include <vector>

#include <fftw3.h>

#include "app_state.hpp"

// ============================================================================
// PL: накопленные спектрально-корреляционные статистики одной подполосы
// (соответствует записи из раздела 17 PDF, поля S_sum_pow..S12_im)
// ============================================================================
struct SubbandStats {
    double sum_pow      = 0.0; // S_ΣΣ[p]
    double diff_pow      = 0.0; // S_ΔΔ[p]
    double diff_sum_re  = 0.0; // Re{S_ΔΣ[p]}
    double diff_sum_im  = 0.0; // Im{S_ΔΣ[p]}
    double s11           = 0.0; // S11[p]
    double s22           = 0.0; // S22[p]
    double s12_re        = 0.0; // Re{S12[p]}
    double s12_im        = 0.0; // Im{S12[p]}
};

// ============================================================================
// PL: приём RX1/RX2, удаление DC, окно, БПФ, формирование Sigma/Delta,
// накопление статистик по подполосам за M кадров, ping-pong буфер (раздел
// 4-14 PDF). Луч зафиксирован на 0 (alpha = 1) — калибровочный коэффициент
// оставлен как задел под будущую компенсацию каналов.
// ============================================================================
class SubbandAccumulator {
public:
    struct Params {
        int fft_size      = 4096; // N_FFT
        int subband_size  = 16;   // B бинов на подполосу
        int accum_frames  = 256;  // M кадров накопления
        int dc_shift_bits = 14;   // alpha = 2^-N для удаления DC, N = 12..16
    };

    explicit SubbandAccumulator(const Params& params);
    ~SubbandAccumulator();

    SubbandAccumulator(const SubbandAccumulator&)            = delete;
    SubbandAccumulator& operator=(const SubbandAccumulator&) = delete;

    // принимает ровно fft_size() отсчётов на канал и накапливает один FFT-кадр
    void push_frame(const Complex* rx1, const Complex* rx2);

    // Калибровка канала 2 по бинам — C2[k]/C1[k] из раздела 19.4 PDF. Множители
    // применяются к спектру RX2 ДО формирования Sigma/Delta: после того как
    // Sigma и Delta сложены, поправка в них уже не вносится.
    // Пустой вектор выключает калибровку; длина обязана быть fft_size().
    void set_channel_correction(const std::vector<Complex>& c2_per_bin);
    bool has_channel_correction() const { return !c2_.empty(); }

    bool has_ready() const { return ready_flag_; }
    // возвращает завершённый блок статистик подполос и сбрасывает флаг готовности
    std::vector<SubbandStats> take_ready();

    int fft_size() const { return params_.fft_size; }
    int subband_size() const { return params_.subband_size; }
    int accum_frames() const { return params_.accum_frames; }
    int num_subbands() const { return num_subbands_; }

private:
    Params params_;
    int    num_subbands_;

    fftwf_complex* in1_ { nullptr };
    fftwf_complex* out1_{ nullptr };
    fftwf_complex* in2_ { nullptr };
    fftwf_complex* out2_{ nullptr };
    fftwf_plan     plan_{ nullptr };

    std::vector<float> hann_;

    // IIR-оценка постоянной составляющей, непрерывная между вызовами push_frame
    // (раздел 5 PDF): mu[n] = mu[n-1] + (x[n]-mu[n-1]) / 2^N
    Complex dc1_{ 0.0f, 0.0f };
    Complex dc2_{ 0.0f, 0.0f };
    float   dc_alpha_;

    // калибровочный коэффициент, применяемый к RX1 перед формированием Sigma/Delta;
    // 1+0j соответствует лучу в нулевом положении без коррекции амплитуды/фазы
    Complex alpha_left_{ 1.0f, 0.0f };

    // C2[k]: перекос каналов, снимаемый с RX2 побиново. Пустой — калибровки нет.
    std::vector<Complex> c2_;

    std::vector<SubbandStats> accum_; // текущий (пишущийся) буфер
    std::vector<SubbandStats> ready_; // последний завершённый буфер
    bool ready_flag_    = false;
    int  frame_in_accum_= 0;
};

// ============================================================================
// PS: моноимпульсное отношение, когерентность, SNR по подполосе (раздел 19
// PDF, пп. 19.4-19.7). Частотная калибровка C1[p],C2[p] и калибровочная
// таблица ρcal(θ,f) — задел на будущие версии, здесь не применяются.
// ============================================================================
struct SubbandResult {
    int   index         = 0;
    float freq_offset_hz= 0.0f; // относительно центральной частоты приёмника
    float rho_re         = 0.0f; // Re{ρ[p]} = Re{Delta/Sigma}
    float rho_im         = 0.0f; // Im{ρ[p]}
    float gamma2         = 0.0f; // когерентность RX1/RX2, 0..1
    float snr_db          = 0.0f; // 10*log10((S_ΣΣ[p]-N_p) / (N_p+eps)), раздел 19.7
    float sigma_dbfs      = 0.0f; // средняя мощность Sigma на бин, дБ от полной шкалы
    bool  valid           = false; // маска пригодности q_p (раздел 19.11, без e_p/s_p — нет калибровочной таблицы)
};

class SubbandEstimator {
public:
    struct Params {
        float snr_min_db   = 3.0f;  // SNR_min из раздела 19.11
        float gamma2_min   = 0.5f;  // gamma^2_min из раздела 19.11
        double min_power   = 0.0;   // P_min из раздела 19.11; порог S_ΣΣ[p], по умолчанию выключен —
                                     // нет калибровки полной шкалы (K_scale, раздел 19.4)
        int   noise_window = 16;    // половина окна Ω(p) в подполосах для локальной медианы N_p
                                     // (раздел 19.7, второй подход — устойчивее к широкополосному сигналу,
                                     // чем глобальная медиана по всем подполосам)
    };

    explicit SubbandEstimator(const Params& params) : params_(params) {}

    // sample_rate_hz нужен только для перевода индекса подполосы в offset по частоте.
    //
    // c2_per_subband — калибровка канала 2 для случая, когда Sigma/Delta в stats
    // сформированы БЕЗ неё (статистики с платы). Пересчёт возможен потому, что
    // PL передаёт полную ковариацию S11/S22/S12, а не только Sigma/Delta:
    //   S_Σ'Σ' = S11 + |c2|²·S22 + 2·Re{conj(c2)·S12}
    //   S_Δ'Δ' = S11 + |c2|²·S22 − 2·Re{conj(c2)·S12}
    //   S_Δ'Σ' = S11 − |c2|²·S22 + 2i·Im{conj(c2)·S12}
    // (c1 = 1: важно только отношение каналов). Пустой вектор — оставить как есть,
    // и именно так сюда приходят локальные блоки: там калибровку уже применил
    // SubbandAccumulator, побиново, и применять её второй раз нельзя.
    std::vector<SubbandResult> estimate(const std::vector<SubbandStats>& stats,
                                         int accum_frames, int subband_size,
                                         int fft_size, double sample_rate_hz,
                                         const std::vector<Complex>& c2_per_subband = {}) const;

private:
    Params params_;
};
