#include "finding.hpp"

#include "paths.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <thread>

DirectionFindingThread::DirectionFindingThread(CircularBuffer<CoherentSample>& df_buf, AppState& state)
    : df_buf_(df_buf), state_(state) {
    SubbandAccumulator::Params acc_params; // N_FFT=4096, B=16, M=128 — раздел 21 PDF
    accumulator_ = std::make_unique<SubbandAccumulator>(acc_params);

    SubbandEstimator::Params est_params;
    estimator_ = std::make_unique<SubbandEstimator>(est_params);

    int n = accumulator_->fft_size();
    batch_.resize(n);
    rx1_.resize(n);
    rx2_.resize(n);
}

void DirectionFindingThread::run() {
    std::cout << "[peleng] direction finding thread started (fft=" << accumulator_->fft_size()
               << ", subbands=" << accumulator_->num_subbands()
               << ", M=" << accumulator_->accum_frames() << ")\n";

    while (state_.running.load()) {
        // Останов журнала обрабатывается здесь, а не только рядом с записью
        // блока: если сигнал пропал, блоки перестают приходить, а кнопка Stop
        // всё равно должна закрыть файл.
        if (state_.peleng_log_stop_requested.exchange(false)) {
            close_log();
            state_.peleng_is_logging.store(false);
        }

        // В режиме сравнения оба пути считаются каждый такт, независимо от
        // того, какой выбран для графика/AppState — переключатель источника
        // ниже влияет только на display, не на то, что здесь запускается.
        bool compare      = state_.peleng_compare_mode.load();
        bool use_network  = state_.peleng_use_network.load();
        bool want_network = compare || use_network;
        bool want_local   = compare || !use_network;

        if (want_network) {
            run_network_step();
        } else if (receiver_) {
            receiver_.reset();
            state_.peleng_net_connected.store(false);
        }

        if (want_local) run_local_step();
    }

    close_log(); // выход из программы во время записи не должен обрывать файл на полустроке
    state_.peleng_is_logging.store(false);

    std::cout << "[peleng] direction finding thread exiting\n";
}

void DirectionFindingThread::run_local_step() {
    bool compare = state_.peleng_compare_mode.load();
    if (compare && have_board_geometry_) {
        // Сравнение просит подполосу N значить одно и то же по обе стороны.
        ensure_local_geometry(board_fft_, board_subband_, board_accum_);
    } else if (!compare) {
        SubbandAccumulator::Params def; // конструктор без сравнения — как раньше
        ensure_local_geometry(def.fft_size, def.subband_size, def.accum_frames);
    } else {
        // Сравнение включено, но геометрия платы ещё не пришла ни разу —
        // считать не на чем сравнивать, ждём первого сетевого блока.
        return;
    }

    size_t frame_size = batch_.size();

    // В режиме сравнения этот шаг делит цикл потока с run_network_step(), и
    // тот не мгновенен (recv() на блок статистик). Одного кадра за вызов было
    // достаточно, пока локальный путь был единственным (цикл крутился без
    // ничего постороннего) — раздельно с сетевым он отстаёт: 128 кадров на
    // блок при вызове раз в сетевой блок означало бы блок раз в 128 сетевых
    // блоков. Поэтому вычерпываем всё, что уже накопилось в df_buf_, за один
    // вызов, а не по кадру.
    bool drained_any = false;
    while (df_buf_.available() >= frame_size) {
        if (!df_buf_.pop_batch(batch_.data(), frame_size)) break;
        drained_any = true;

        for (size_t i = 0; i < frame_size; ++i) {
            rx1_[i] = batch_[i].rx1;
            rx2_[i] = batch_[i].rx2;
        }

        // Поправка ставится до кадра: применить её к уже сложенным Sigma/Delta нельзя.
        update_calibration(static_cast<double>(state_.center_freq_hz.load()),
                            static_cast<double>(state_.sample_rate_hz.load()),
                            accumulator_->fft_size(), accumulator_->subband_size());
        accumulator_->set_channel_correction(c2_bins_);

        accumulator_->push_frame(rx1_.data(), rx2_.data());

        if (!accumulator_->has_ready()) continue;

        // Пустой вектор для оценщика: локальные блоки уже откалиброваны побиново,
        // и вторая поправка удвоила бы её.
        estimate_and_publish(accumulator_->take_ready(),
                              accumulator_->accum_frames(), accumulator_->subband_size(),
                              accumulator_->fft_size(),
                              static_cast<double>(state_.sample_rate_hz.load()),
                              static_cast<double>(state_.center_freq_hz.load()),
                              {},
                              /*is_display_source=*/ !state_.peleng_use_network.load(),
                              /*source_id=*/ 0);
    }

    if (!drained_any) std::this_thread::sleep_for(std::chrono::microseconds(10));
}

void DirectionFindingThread::run_network_step() {
    uint16_t port = state_.peleng_net_port.load();
    if (receiver_ && receiver_->port() != port) {
        receiver_.reset(); // UI changed the port — rebind
        state_.peleng_net_connected.store(false);
    }
    if (!receiver_)
        receiver_ = std::make_unique<NetworkSubbandReceiver>(port);

    SubbandWireHeader header{};
    bool got = receiver_->poll_block(net_stats_, header, 100);
    state_.peleng_net_connected.store(receiver_->is_connected());
    if (!got) return;

    // The board tells us the geometry it actually used; nothing here has to
    // match the local SubbandAccumulator's parameters — unless compare mode
    // asked the local side to follow it (see run_local_step/ensure_local_geometry).
    have_board_geometry_ = true;
    board_fft_           = static_cast<int>(header.fft_size);
    board_subband_       = static_cast<int>(header.subband_size);
    board_accum_         = static_cast<int>(header.accum_frames);

    // Геометрию и настройку задаёт плата, а не UI: калибровка считается под то,
    // что реально пришло в заголовке блока.
    update_calibration(static_cast<double>(header.center_freq_hz),
                        static_cast<double>(header.sample_rate_hz),
                        static_cast<int>(header.fft_size),
                        static_cast<int>(header.subband_size));

    // Здесь поправка идёт целиком: отсчёты с платы не проходили через поток
    // захвата, значит константу с них тоже никто не снимал.
    estimate_and_publish(net_stats_,
                          static_cast<int>(header.accum_frames),
                          static_cast<int>(header.subband_size),
                          static_cast<int>(header.fft_size),
                          static_cast<double>(header.sample_rate_hz),
                          static_cast<double>(header.center_freq_hz),
                          c2_subbands_,
                          /*is_display_source=*/ state_.peleng_use_network.load(),
                          /*source_id=*/ 1);
}

void DirectionFindingThread::estimate_and_publish(const std::vector<SubbandStats>& stats,
                                                    int accum_frames, int subband_size, int fft_size,
                                                    double sample_rate_hz, double center_hz,
                                                    const std::vector<Complex>& c2_per_subband,
                                                    bool is_display_source, int source_id) {
    std::vector<SubbandResult> results =
        estimator_->estimate(stats, accum_frames, subband_size, fft_size, sample_rate_hz,
                              c2_per_subband);

    // Только выбранный в UI источник рисуется на графике/публикуется в AppState —
    // иначе в режиме сравнения они бы перетирали друг друга в общих полях.
    if (is_display_source)
        publish_results(results, accum_frames, subband_size, fft_size, center_hz);

    // А в CSV попадает каждый посчитанный блок независимо от того, что сейчас
    // на экране: без сравнения здесь оказывается только один источник (второй
    // просто не считается, см. run()), с ним — оба, размеченные source_id.
    log_block(source_id, results, static_cast<int>(results.size()), sample_rate_hz, center_hz);
}

// Пересоздаёт локальный accumulator_ под геометрию, под которую сейчас нужно
// считать. Дёшево, когда она не поменялась: сравнение параметров и выход.
void DirectionFindingThread::ensure_local_geometry(int fft_size, int subband_size, int accum_frames) {
    if (accumulator_ &&
        accumulator_->fft_size() == fft_size &&
        accumulator_->subband_size() == subband_size &&
        accumulator_->accum_frames() == accum_frames)
        return;

    SubbandAccumulator::Params p;
    p.fft_size     = fft_size;
    p.subband_size = subband_size;
    p.accum_frames = accum_frames;
    accumulator_   = std::make_unique<SubbandAccumulator>(p);

    int n = accumulator_->fft_size();
    batch_.resize(n);
    rx1_.resize(n);
    rx2_.resize(n);
}

// ============================================================================
// Калибровка каналов C1[p]/C2[p]
// ============================================================================
//
// Разделение труда между потоками не произвольное, а следует из того, чем
// является каждая часть поправки. Константа перекоса — это поворот, его можно
// применить к отсчёту, и поток захвата это уже делает (capture.cpp, cal_active_).
// Задержка — это фильтр, одним множителем на отсчёт она не применяется, зато
// тривиально применяется к бину. Поэтому:
//   локальный путь  — здесь только задержечная часть, константу уже сняли;
//   сетевой путь    — здесь вся поправка, потому что статистики пришли с платы
//                     и потока захвата не видели.
void DirectionFindingThread::update_calibration(double center_hz, double sample_rate_hz,
                                                  int fft_size, int subband_size) {
    bool     enabled = state_.rx_cal_enabled.load();
    uint32_t gen     = state_.rx_cal_generation.load();
    int      gain    = state_.gain_db.load();

    bool same = enabled == cal_key_enabled_ && gen == cal_key_gen_
             && center_hz == cal_key_freq_ && sample_rate_hz == cal_key_rate_
             && gain == cal_key_gain_ && fft_size == cal_key_fft_
             && subband_size == cal_key_sub_;
    if (same) return;

    cal_key_enabled_ = enabled;
    cal_key_gen_     = gen;
    cal_key_freq_    = center_hz;
    cal_key_rate_    = sample_rate_hz;
    cal_key_gain_    = gain;
    cal_key_fft_     = fft_size;
    cal_key_sub_     = subband_size;

    c2_bins_.clear();
    c2_subbands_.clear();

    if (!enabled || fft_size <= 0 || subband_size <= 0 || sample_rate_hz <= 0.0) {
        state_.peleng_cal_active.store(false);
        state_.peleng_cal_delay_ns.store(0.0f);
        return;
    }

    if (!cal_.valid() || gen != cal_generation_ || !cal_load_tried_) {
        std::string path, error;
        {
            std::lock_guard<std::mutex> lock(state_.rx_cal_mutex);
            path = state_.rx_cal_path;
        }
        bool ok = cal_.load(path, error);
        cal_load_tried_  = true;
        cal_generation_  = gen;
        std::cout << (ok ? "[peleng] calibration loaded: " : "[peleng] calibration not loaded: ")
                   << (ok ? cal_.source() : error) << "\n";
    }

    if (!cal_.valid()) {
        state_.peleng_cal_active.store(false);
        state_.peleng_cal_delay_ns.store(0.0f);
        return;
    }

    // Смещение бина от гетеродина: бины идут в естественном порядке БПФ, вторая
    // половина — это отрицательные частоты.
    auto offset_of = [&](int k) {
        int signed_k = (k <= fft_size / 2) ? k : k - fft_size;
        return signed_k * sample_rate_hz / fft_size;
    };

    // Задержечная часть — это отношение поправки со смещением к поправке на
    // самом гетеродине. Считать её отдельной формулой значило бы завести второе
    // место, где знак задержки может разойтись с таблицей.
    Complex base = cal_.correction(center_hz, gain, 0.0);

    c2_bins_.resize(static_cast<size_t>(fft_size));
    for (int k = 0; k < fft_size; ++k)
        c2_bins_[k] = cal_.correction(center_hz, gain, offset_of(k)) / base;

    int num_subbands = fft_size / subband_size;
    c2_subbands_.resize(static_cast<size_t>(std::max(num_subbands, 0)));
    for (int p = 0; p < num_subbands; ++p) {
        int k_center = p * subband_size + subband_size / 2;
        c2_subbands_[p] = cal_.correction(center_hz, gain, offset_of(k_center));
    }

    state_.peleng_cal_active.store(true);
    state_.peleng_cal_delay_ns.store(cal_.delay_ns(center_hz, gain));
    std::cout << "[peleng] C1/C2 applied: " << center_hz / 1e6 << " MHz, gain " << gain
               << " dB, delay " << cal_.delay_ns(center_hz, gain) << " ns"
               << (cal_.in_range(center_hz, gain) ? "" : " (outside the table, clamped)") << "\n";
}

void DirectionFindingThread::publish_results(const std::vector<SubbandResult>& results,
                                               int accum_frames, int subband_size, int fft_size,
                                               double center_hz) {
    size_t n = results.size();

    state_.peleng_fft_size.store(fft_size);
    state_.peleng_subband_size.store(subband_size);
    state_.peleng_accum_frames.store(accum_frames);

    {
        std::lock_guard<std::mutex> lock(state_.peleng_mutex);

        state_.peleng_freq_mhz.resize(n);
        state_.peleng_rho_re.resize(n);
        state_.peleng_rho_im.resize(n);
        state_.peleng_gamma2.resize(n);
        state_.peleng_snr_db.resize(n);
        state_.peleng_sigma_dbfs.resize(n);
        state_.peleng_valid.resize(n);

        // fftshift at subband granularity: results[] is in natural FFT bin order
        // (DC, +freq up to Nyquist, then wraps to the most negative offset), which
        // would plot as a discontinuous jump. Reorder to a monotonic frequency axis,
        // same idea as ProcessingThread::convert_to_dbm's sample-level fftshift.
        size_t half = n / 2;
        for (size_t p = 0; p < n; ++p) {
            size_t dst = (p < half) ? (p + half) : (p - half);
            const SubbandResult& r = results[p];
            state_.peleng_freq_mhz[dst]   = static_cast<float>((center_hz + r.freq_offset_hz) / 1e6);
            state_.peleng_rho_re[dst]     = r.rho_re;
            state_.peleng_rho_im[dst]     = r.rho_im;
            state_.peleng_gamma2[dst]     = r.gamma2;
            state_.peleng_snr_db[dst]     = r.snr_db;
            state_.peleng_sigma_dbfs[dst] = r.sigma_dbfs;
            state_.peleng_valid[dst]      = r.valid ? 1 : 0;
        }

        state_.peleng_has_data.store(n > 0);
        state_.peleng_update_count.fetch_add(1);
    }
}

// Файловый ввод-вывод — вызывается для каждого посчитанного блока, что от
// локального пути, что от сетевого, независимо от того, какой из них сейчас
// выбран для отображения (см. estimate_and_publish).
void DirectionFindingThread::log_block(int source_id, const std::vector<SubbandResult>& results,
                                        int num_subbands, double sample_rate_hz, double center_hz) {
    handle_log_start(num_subbands, sample_rate_hz, center_hz);
    if (log_file_.is_open()) write_log_block(source_id, results, center_hz);
}

// ============================================================================
// Журнал подполосных характеристик (CSV)
// ============================================================================

// Открытие отложено до момента, когда готов очередной блок: только тогда
// известна настоящая геометрия накопления (в сетевом режиме её задаёт плата),
// и шапка файла описывает именно то, что в него пишется.
void DirectionFindingThread::handle_log_start(int num_subbands, double sample_rate_hz, double center_hz) {
    if (!state_.peleng_log_start_requested.exchange(false)) return;

    close_log(); // повторный «Start» без «Stop» = начать новый файл
    std::string path;
    {
        std::lock_guard<std::mutex> lock(state_.peleng_log_mutex);
        path = state_.peleng_log_path;
    }
    bool ok = open_log(path, num_subbands, sample_rate_hz, center_hz);
    state_.peleng_log_error.store(!ok);
    state_.peleng_is_logging.store(ok);
}

bool DirectionFindingThread::open_log(const std::string& path, int num_subbands,
                                        double sample_rate_hz, double center_hz) {
    if (path.empty()) {
        std::cerr << "[peleng] log: empty path\n";
        return false;
    }

    time_t now = time(nullptr);
    char   ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", localtime(&now));

    std::filesystem::path full = resolve_data_path(path);
    if (std::filesystem::exists(full)) {
        // Start с тем же путём, что уже занят предыдущей записью, почти
        // всегда означает «забыл переименовать», а не «хочу стереть старую» —
        // молчаливый trunc() однажды уже съел запись на шуме. Вместо этого
        // дописываем время к имени, как save_iq_file() делает для IQ-файлов.
        char suffix[32];
        strftime(suffix, sizeof(suffix), "_%Y%m%d_%H%M%S", localtime(&now));
        full = full.parent_path() / (full.stem().string() + suffix + full.extension().string());
        std::cerr << "[peleng] log: " << path << " already exists, writing to "
                  << full.filename().string() << " instead\n";
    }

    log_file_.open(full, std::ios::out | std::ios::trunc);
    if (!log_file_) {
        std::cerr << "[peleng] log: cannot open " << full << "\n";
        return false;
    }

    log_started_at_  = std::chrono::steady_clock::now();
    log_block_index_ = 0;
    state_.peleng_log_rows_written.store(0);
    state_.peleng_log_blocks_written.store(0);

    // Шапка — комментарии с геометрией накопления, дальше обычная таблица.
    // numpy.genfromtxt и pandas.read_csv одинаково умеют пропускать '#'.
    // Значения в шапке — на момент открытия файла: частоту и режим можно
    // переключить по ходу записи, поэтому частота каждой строки пишется
    // отдельной колонкой freq_mhz и на неё и надо опираться.
    // source в шапке — только описание того, что было выбрано на момент
    // открытия файла: с включённым сравнением в данных будут оба источника,
    // и различать их надо по колонке source в самих строках, не по шапке.
    std::string source_desc = state_.peleng_compare_mode.load()
        ? "compare (local+network)"
        : (state_.peleng_use_network.load() ? "network" : "local");

    log_file_ << "# spectrum_analyzer peleng log v1\n"
              << "# started=" << ts << "\n"
              << "# source=" << source_desc << "\n"
              << "# center_hz=" << static_cast<long long>(center_hz) << "\n"
              << "# sample_rate_hz=" << static_cast<long long>(sample_rate_hz) << "\n"
              << "# fft_size=" << state_.peleng_fft_size.load() << "\n"
              << "# subband_size=" << state_.peleng_subband_size.load() << "\n"
              << "# accum_frames=" << state_.peleng_accum_frames.load() << "\n"
              << "# num_subbands=" << num_subbands << "\n"
              // Без этой строки два журнала одного и того же сигнала, снятые до
              // и после включения калибровки, выглядят как разные измерения.
              << "# channel_cal=" << (state_.peleng_cal_active.load() ? "on" : "off")
              << " delay_ns=" << state_.peleng_cal_delay_ns.load() << "\n"
              // source: 0 = local (PC model), 1 = network (board PL) — always
              // present so a comparison and a plain single-source log share
              // one schema; peleng_analysis.py splits on it when both appear.
              << "source,block,t_sec,subband,freq_mhz,rho_re,rho_im,gamma2,snr_db,sigma_dbfs,valid\n";

    std::cout << "[peleng] log started: " << full << "\n";
    return true;
}

void DirectionFindingThread::close_log() {
    if (!log_file_.is_open()) return;
    log_file_.close();
    std::cout << "[peleng] log stopped: " << state_.peleng_log_rows_written.load()
               << " rows in " << state_.peleng_log_blocks_written.load() << " blocks\n";
}

void DirectionFindingThread::write_log_block(int source_id, const std::vector<SubbandResult>& results,
                                              double center_hz) {
    size_t n    = results.size();
    size_t half = n / 2;
    if (n == 0) return;

    double t_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - log_started_at_).count();

    // Выбранная подполоса задана в индексах ПОСЛЕ fftshift (как их видит UI),
    // а results[] идёт в натуральном порядке бинов — поэтому фильтруем по dst,
    // а не по p.
    bool selected_only = state_.peleng_log_selected_only.load();
    int  selected      = state_.peleng_selected_subband.load();
    if (selected_only && (selected < 0 || selected >= static_cast<int>(n))) return;

    log_row_buf_.clear();
    char   line[256];
    size_t rows = 0;

    for (size_t p = 0; p < n; ++p) {
        size_t dst = (p < half) ? (p + half) : (p - half);
        if (selected_only && static_cast<int>(dst) != selected) continue;

        const SubbandResult& r = results[p];
        int len = snprintf(line, sizeof(line),
                            "%d,%llu,%.6f,%zu,%.6f,%.6g,%.6g,%.5f,%.3f,%.3f,%d\n",
                            source_id,
                            static_cast<unsigned long long>(log_block_index_), t_sec, dst,
                            (center_hz + r.freq_offset_hz) / 1e6,
                            r.rho_re, r.rho_im, r.gamma2, r.snr_db, r.sigma_dbfs,
                            r.valid ? 1 : 0);
        if (len > 0) {
            log_row_buf_.append(line, static_cast<size_t>(len));
            ++rows;
        }
    }

    log_file_.write(log_row_buf_.data(), static_cast<std::streamsize>(log_row_buf_.size()));
    if (!log_file_) {
        std::cerr << "[peleng] log: write failed, stopping\n";
        close_log();
        state_.peleng_is_logging.store(false);
        state_.peleng_log_error.store(true);
        return;
    }

    ++log_block_index_;
    state_.peleng_log_rows_written.fetch_add(rows);
    state_.peleng_log_blocks_written.fetch_add(1);
}
