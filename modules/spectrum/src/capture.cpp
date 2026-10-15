#include "capture.hpp"
#include "paths.hpp"
#include <iio.h>
// #include <iio/iio.h>
// #include <iio/iio-debug.h>
// #include <ad9361.h>
#include <iostream>
#include <fstream>
#include <ctime>
#include <filesystem>
#include <thread>
#include <cmath>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

using namespace std;

static const char* PORT_OPTIONS[] = { "A_BALANCED", "B_BALANCED", "C_BALANCED",
                                      "A_N", "A_P", "B_N", "B_P" };

// Пишет атрибут только если значение реально меняется. Запись sampling_frequency
// / rf_bandwidth / LO на AD9361 перезапускает калибровки чипа и на мгновение рвёт
// поток ADC в PL (см. bctrl adrst) — параллельно читающий PL-регистры fzstats_send
// в этот момент ловит IRQ_ADC_V/FFT_ERR. Каждое лишнее подключение GUI к уже
// настроенной так же плате не должно дёргать чип впустую.
static void write_longlong_if_changed(struct iio_channel* chn, const char* attr, long long want) {
    long long current = 0;
    if (iio_channel_attr_read_longlong(chn, attr, &current) == 0 && current == want) return;
    iio_channel_attr_write_longlong(chn, attr, want);
}

static void write_string_if_changed(struct iio_channel* chn, const char* attr, const char* want) {
    char current[64];
    if (iio_channel_attr_read(chn, attr, current, sizeof(current)) > 0 &&
        strcmp(current, want) == 0) {
        return;
    }
    iio_channel_attr_write(chn, attr, want);
}

PlutoCaptureThread::PlutoCaptureThread(
    CircularBuffer<CoherentSample>& fft_buf,
    CircularBuffer<CoherentSample>& df_buf,
    CircularBuffer<CoherentSample>& demod_buf,
    AppState& state)
    : fft_buf_(fft_buf), df_buf_(df_buf), demod_buf_(demod_buf), state_(state), clip_monitor_(state) {}

PlutoCaptureThread::~PlutoCaptureThread() {
    close_device();
}

void PlutoCaptureThread::run() {
    reload_calibration();

    cout << "[pluto] thread started, waiting for connect request...\n";

    while (state_.running.load()) {

        if (state_.scan_requested.load()) {
            state_.scan_requested.store(false);
            state_.is_scanning.store(true);
            cout << "[pluto] Scanning for available IIO contexts...\n";

            struct iio_scan_context *scan_ctx = iio_create_scan_context(nullptr, 0);
            if (scan_ctx) {
                struct iio_context_info **info_list = nullptr;
                ssize_t ret = iio_scan_context_get_info_list(scan_ctx, &info_list);
                
                std::vector<std::string> new_uris;
                new_uris.push_back("ip:192.168.2.1");
                new_uris.push_back("local");

                if (ret > 0) {
                    for (ssize_t i = 0; i < ret; ++i) {
                        const char* uri = iio_context_info_get_uri(info_list[i]);
                        const char* description = iio_context_info_get_description(info_list[i]);
                        
                        if (uri) {
                            std::string entry = uri;
                            if (description && strlen(description) > 0) {
                                entry += " (" + std::string(description) + ")";
                            }
                            new_uris.push_back(uri);
                        }
                    }
                    iio_context_info_list_free(info_list);
                }
                iio_scan_context_destroy(scan_ctx);

                {
                    std::lock_guard<std::mutex> lock(state_.uri_list_mutex);
                    state_.found_uris = std::move(new_uris);
                }
            } else {
                cerr << "[pluto] Failed to create scan context\n";
            }
            state_.is_scanning.store(false);
        }

        // Разрыв обрабатываем раньше подключения и гасим заодно connect_requested:
        // иначе запрос, поставленный в том же кадре (например, правкой настройки),
        // тут же поднял бы связь обратно и кнопка выглядела бы неработающей.
        if (state_.disconnect_requested.load()) {
            state_.disconnect_requested.store(false);
            state_.connect_requested.store(false);
            state_.is_connected.store(false);
            clip_monitor_.end();
            close_device();
            cout << "[pluto] disconnected by user\n";
        }

        if (state_.rx_cal_reload_requested.exchange(false)) {
            reload_calibration();
        }

        if (state_.ad9361_reg_dump_requested.exchange(false)) {
            dump_ad9361_registers();
        }

        if (state_.connect_requested.load()) {
            state_.connect_requested.store(false);
            state_.connect_in_progress.store(true);
            state_.is_connected.store(false);

            close_device();
            
            cout << "[pluto] attempting connection...\n";
            if (open_device() && configure_device()) {
                state_.is_connected.store(true);
                // Счётчики перегрузки живут ровно столько, сколько идёт поток:
                // «зажатые отсчёты за прошлое подключение, на другом усилении»
                // — это не то, о чём стоит предупреждать.
                clip_monitor_.begin();
                // Тем же поводом чистим историю фазы: значения за прошлое
                // подключение (другая частота, другое усиление) только мешают
                // читать график.
                phase_stream_time_s_ = 0.0;
                {
                    lock_guard<mutex> lock(state_.phase_hist_mutex);
                    state_.phase_hist_time_s.clear();
                    state_.phase_hist_deg.clear();
                }
                current_freq_ = state_.center_freq_hz.load();
                current_sample_rate_ = state_.sample_rate_hz.load();
                current_mode_ = state_.chan_mode.load();
                cout << "[pluto] successfully connected and streaming\n";
            } else {
                close_device();
                cerr << "[pluto] connection failed\n";
            }
            state_.connect_in_progress.store(false);
        }

        

        if (state_.is_connected.load() && rx_buf_) {
            uint64_t new_freq = state_.center_freq_hz.load();
            if (new_freq != current_freq_) {
                struct iio_channel* lo_chan = iio_device_find_channel(phy_dev_, "altvoltage0", true);
                if (lo_chan) {
                    iio_channel_attr_write_longlong(lo_chan, "frequency", new_freq);
                    current_freq_ = new_freq;
                }
            }

            // Усиление, в отличие от частоты дискретизации и полосы, меняется на
            // ходу: буфер пересоздавать не нужно, поэтому и переподключения по
            // нему нет — ползунок действует сразу.
            apply_gain(false);
            poll_gain_readback();

            ssize_t n_bytes = iio_buffer_refill(rx_buf_);
            if (n_bytes < 0) {
                log_error("iio_buffer_refill", n_bytes);
                state_.is_connected.store(false);
                clip_monitor_.end();
                close_device();
                continue;
            }

            process_rx_buffer();
            clip_monitor_.tick();

            if (state_.save_requested.load()) {
                save_iq_file();
                state_.save_requested.store(false);
            }
        } else {
            this_thread::sleep_for(chrono::milliseconds(50));
        }
    }

    close_device();
    cout << "[pluto] exiting\n";
}

bool PlutoCaptureThread::open_device() {
    string ip;
    {
        lock_guard<mutex> lock(state_.connection_mutex);
        ip = state_.pluto_ip;
    }

    size_t space_pos = ip.find(' ');
    if (space_pos != string::npos) {
        ip = ip.substr(0, space_pos);
    }

    if (ip.empty() || ip == "local") {
        ctx_ = iio_create_default_context();
    } else {
        ctx_ = iio_create_context_from_uri(ip.c_str());
    }

    if (!ctx_) {
        cerr << "[pluto] failed to create libiio context for: " << ip << "\n";
        return false;
    }

    phy_dev_ = iio_context_find_device(ctx_, "ad9361-phy");
    rx_dev_  = iio_context_find_device(ctx_, "cf-ad9361-lpc");

    if (!phy_dev_ || !rx_dev_) {
        cerr << "[pluto] failed to find AD9361 devices\n";
        return false;
    }

    return true;
}

bool PlutoCaptureThread::configure_device() {

    current_freq_ = state_.center_freq_hz.load();
    current_sample_rate_ = state_.sample_rate_hz.load();
    current_mode_ = state_.chan_mode.load();


    uint32_t fs = current_sample_rate_;
    uint32_t bw = state_.bandwidth_hz.load();
    int port_idx = state_.rf_port_idx.load();
    PlutoChannelMode mode = current_mode_;

    // Первым делом: инверсия Rx2 требует переинициализации чипа и снесла бы
    // всё, что записано ниже.
    apply_phase_inversion();

    // Каналы ищем сразу после инверсии и до первой записи: указатели живут в
    // контексте libiio и переинициализацию чипа переживают, а вот всё, что
    // настраивается ниже, без них — тихий no-op.
    phy_chn0_ = iio_device_find_channel(phy_dev_, "voltage0", false);
    phy_chn1_ = iio_device_find_channel(phy_dev_, "voltage1", false);

    // Двухканальный режим — это сравнение трактов между собой (пеленг, кросс-фаза),
    // а АРУ ведёт каналы независимо и разводит их по индексам таблицы усиления.
    // Разное усиление на трактах — уже систематическая ошибка измерения, а на
    // нижних индексах вдобавок обходится внутренний LNA, и это доворачивает фазу
    // канала ещё на 180°. Поэтому при подключении в Both усиление принудительно
    // ручное и общее; вернуть АРУ можно тут же ползунками, но уже осознанно.
    if (mode == PlutoChannelMode::Both &&
        (state_.gain_mode.load() != GainMode::Manual || !state_.gain_link_channels.load())) {
        state_.gain_mode.store(GainMode::Manual);
        state_.gain_link_channels.store(true);
        cout << "[pluto] dual-channel mode: gain forced to manual and linked "
                "(AGC decorrelates the two receivers)\n";
    }

    // Настройка Rx1 (voltage0)
    if (phy_chn0_) {
        write_string_if_changed(phy_chn0_, "rf_port_select", PORT_OPTIONS[port_idx]);
        write_longlong_if_changed(phy_chn0_, "rf_bandwidth", bw);
        write_longlong_if_changed(phy_chn0_, "sampling_frequency", fs);
    }

    // Настройка Rx2 (voltage1) для двухканального режима
    if (phy_chn1_ && (mode == PlutoChannelMode::Channel_2 || mode == PlutoChannelMode::Both)) {
        write_string_if_changed(phy_chn1_, "rf_port_select", PORT_OPTIONS[port_idx]);
        write_longlong_if_changed(phy_chn1_, "rf_bandwidth", bw);
        write_longlong_if_changed(phy_chn1_, "sampling_frequency", fs);
    }

    // Настройка гетеродина (LO)
    struct iio_channel* lo_chn = iio_device_find_channel(phy_dev_, "altvoltage0", true);
    if (!lo_chn) return false;
    write_longlong_if_changed(lo_chn, "frequency", state_.center_freq_hz.load());

    // Конфигурация для стабильной фазы: отключаем адаптивные алгоритмы,
    // которые меняют фазу в зависимости от уровня сигнала (AM-to-PM эффект).
    // Это необходимо для калибровки и измерений фазы между каналами.
    // Идёт последней из настроек тракта: и запись полосы с частотой
    // дискретизации, и перестройка LO перезапускают калибровки чипа, поэтому
    // отключать слежение раньше них смысла нет.
    if (state_.phase_stable_mode.load()) {
        configure_phase_stable();
    }

    // Усиление — после полосы, частоты дискретизации и LO: индекс в таблице
    // усиления и пороги АРУ чип пересчитывает под текущую конфигурацию тракта,
    // а сама таблица у AD9361 своя на каждый диапазон LO.
    apply_gain(true);

    // --- Изменение: ad9361_set_bb_rate больше не требуется, ---
    // --- так как sampling_frequency уже записана в каналы выше ---

    // Поиск каналов данных цифровой шины
    rx_i0_ = iio_device_find_channel(rx_dev_, "voltage0", false);
    rx_q0_ = iio_device_find_channel(rx_dev_, "voltage1", false);
    rx_i1_ = iio_device_find_channel(rx_dev_, "voltage2", false);
    rx_q1_ = iio_device_find_channel(rx_dev_, "voltage3", false);

    // Включаем каналы в зависимости от выбранного режима
    if (mode == PlutoChannelMode::Channel_1 || mode == PlutoChannelMode::Both) {
        if (rx_i0_) iio_channel_enable(rx_i0_);
        if (rx_q0_) iio_channel_enable(rx_q0_);
    }
    if (mode == PlutoChannelMode::Channel_2 || mode == PlutoChannelMode::Both) {
        if (rx_i1_) iio_channel_enable(rx_i1_);
        if (rx_q1_) iio_channel_enable(rx_q1_);
    }

    // Размер буфера: берем 1/50 секунды от текущей частоты дискретизации
    unsigned int buffer_samples = fs / 50;
    rx_buf_ = iio_device_create_buffer(rx_dev_, buffer_samples, false);
    if (!rx_buf_) {
        cerr << "[pluto] failed to create RX buffer\n";
        return false;
    }

    return true;
}

// Инверсия Rx2 у AD9361: без неё второй приёмник идёт в противофазе первому
// (измерено 180° ± 3°). Драйвер держит её в platform data, а не в обычном
// атрибуте канала, поэтому переключается она через debugfs и вступает в силу
// только после initialize=1.
void PlutoCaptureThread::apply_phase_inversion() {
    static const char* kAttr = "adi,rx1-rx2-phase-inversion-enable";

    state_.rx_phase_inversion_applied.store(false);
    if (!phy_dev_) return;

    // Прошивки без debugfs у ad9361-phy этого атрибута не отдают. Это не отказ
    // подключения — просто инверсию на такой плате не переключить, и UI должен
    // сказать об этом вместо того, чтобы показывать галочку работающей.
    if (!iio_device_find_debug_attr(phy_dev_, kAttr)) {
        state_.rx_phase_inversion_supported.store(false);
        cerr << "[pluto] " << kAttr << " not available on this board; "
                "Rx2 phase inversion left as the firmware set it\n";
        return;
    }
    state_.rx_phase_inversion_supported.store(true);

    bool want = state_.rx_phase_inversion.load();

    // Если чип уже в нужном состоянии — не трогаем: initialize перезапускает
    // тракт целиком, и делать это на каждом переподключении незачем.
    long long current = 0;
    if (iio_device_debug_attr_read_longlong(phy_dev_, kAttr, &current) == 0 &&
        (current != 0) == want) {
        state_.rx_phase_inversion_applied.store(want);
        return;
    }

    int ret = iio_device_debug_attr_write_longlong(phy_dev_, kAttr, want ? 1 : 0);
    if (ret < 0) {
        log_error(string("debug attr ") + kAttr, ret);
        return;
    }

    ret = iio_device_debug_attr_write_longlong(phy_dev_, "initialize", 1);
    if (ret < 0) {
        log_error("debug attr initialize", ret);
        return;
    }
    // Драйвер переинициализирует чип синхронно, но LO и АРУ поднимаются уже
    // после возврата из записи; пауза — чтобы настройка ниже легла на
    // устоявшийся тракт, а не в середину его перезапуска.
    this_thread::sleep_for(chrono::milliseconds(200));

    state_.rx_phase_inversion_applied.store(want);
    cout << "[pluto] Rx2 phase inversion " << (want ? "enabled" : "disabled") << "\n";
}

void PlutoCaptureThread::configure_phase_stable() {
    // Хватает одного канала: у драйвера ad9361 эти три флага общие на чип,
    // несмотря на то что торчат наружу атрибутами канала. Требовать оба —
    // значит молча ничего не сделать там, где второго тракта просто нет.
    if (!phy_dev_ || (!phy_chn0_ && !phy_chn1_)) {
        cerr << "[pluto] phase-stable mode requested but no phy channel available\n";
        return;
    }

    cout << "[pluto] configuring phase-stable mode (disabling adaptive algorithms)...\n";

    struct iio_channel* channels[] = { phy_chn0_, phy_chn1_ };
    const char* channel_names[] = { "voltage0", "voltage1" };

    for (int i = 0; i < 2; i++) {
        struct iio_channel* chn = channels[i];
        const char* name = channel_names[i];

        if (!chn) continue;

        int ret = 0;

        ret = iio_channel_attr_write_bool(chn, "rf_dc_offset_tracking_en", false);
        if (ret < 0) {
            log_error(string(name) + ": rf_dc_offset_tracking_en", ret);
        } else {
            cout << "  [" << name << "] rf_dc_offset_tracking_en = 0 (disabled)\n";
        }

        ret = iio_channel_attr_write_bool(chn, "bb_dc_offset_tracking_en", false);
        if (ret < 0) {
            log_error(string(name) + ": bb_dc_offset_tracking_en", ret);
        } else {
            cout << "  [" << name << "] bb_dc_offset_tracking_en = 0 (disabled)\n";
        }

        ret = iio_channel_attr_write_bool(chn, "quadrature_tracking_en", false);
        if (ret < 0) {
            log_error(string(name) + ": quadrature_tracking_en", ret);
        } else {
            cout << "  [" << name << "] quadrature_tracking_en = 0 (disabled)\n";
        }
    }

    cout << "[pluto] phase-stable configuration complete\n";
}

const char* PlutoCaptureThread::gain_mode_attr(GainMode mode) {
    switch (mode) {
        case GainMode::Manual:     return "manual";
        case GainMode::FastAttack: return "fast_attack";
        case GainMode::Hybrid:     return "hybrid";
        case GainMode::SlowAttack:
        default:                   return "slow_attack";
    }
}

void PlutoCaptureThread::apply_gain(bool force) {
    if (!phy_dev_) return;

    GainMode mode  = state_.gain_mode.load();
    // Раздельное усиление имеет смысл только когда работают оба тракта: в
    // одноканальных режимах на экране один ползунок, и он должен управлять тем
    // приёмником, который включён, — какой бы из двух это ни был.
    bool     link  = state_.gain_link_channels.load() || current_mode_ != PlutoChannelMode::Both;
    int      gain1 = state_.gain_db.load();
    int      gain2 = link ? gain1 : state_.gain_db2.load();

    gain1 = max(AppState::GAIN_DB_MIN, min(AppState::GAIN_DB_MAX, gain1));
    gain2 = max(AppState::GAIN_DB_MIN, min(AppState::GAIN_DB_MAX, gain2));

    bool mode_changed = force || mode != current_gain_mode_;
    // Уровень имеет смысл только там, где индекс усиления держим мы. При смене
    // режима переписываем его и без изменения самого числа: выход из АРУ
    // оставляет в чипе тот индекс, на котором она остановилась, а не наш.
    bool manual_gain  = (mode == GainMode::Manual || mode == GainMode::Hybrid);

    // Порядок важен: hardwaregain в режиме АРУ драйвер отклоняет (-EBUSY),
    // поэтому режим ставится первым, и только потом уровень.
    auto apply_to = [&](struct iio_channel* chn, int gain_db, int& current_gain_db, const char* name) {
        if (!chn) return;
        if (mode_changed) {
            int ret = iio_channel_attr_write(chn, "gain_control_mode", gain_mode_attr(mode));
            if (ret < 0) log_error(string("gain_control_mode (") + name + ")", ret);
        }
        if (manual_gain && (mode_changed || gain_db != current_gain_db)) {
            int ret = iio_channel_attr_write_longlong(chn, "hardwaregain", gain_db);
            if (ret < 0) log_error(string("hardwaregain (") + name + ")", ret);
        }
        current_gain_db = gain_db;
    };

    apply_to(phy_chn0_, gain1, current_gain_db_, "Rx1");
    if (current_mode_ == PlutoChannelMode::Channel_2 || current_mode_ == PlutoChannelMode::Both)
        apply_to(phy_chn1_, gain2, current_gain_db2_, "Rx2");
    else
        current_gain_db2_ = gain2;

    current_gain_mode_ = mode;
}

// Фактическое усиление и уровень на входе — обратно в UI. Опрашивается редко и
// по времени, а не каждый буфер: на сетевом libiio каждое чтение атрибута —
// отдельный запрос к плате, а цифры эти нужны глазу, а не обработке.
void PlutoCaptureThread::poll_gain_readback() {
    using namespace std::chrono;
    auto now = steady_clock::now();
    if (now - last_gain_poll_ < milliseconds(250)) return;
    last_gain_poll_ = now;

    if (!phy_chn0_) {
        state_.gain_readback_valid.store(false);
        return;
    }

    auto read_pair = [](struct iio_channel* chn, std::atomic<float>& gain_out, std::atomic<float>& rssi_out) {
        if (!chn) return false;
        // read_double, а не read_longlong: усиление драйвер отдаёт то целым
        // ("71"), то с дробной частью — разбор целым на втором варианте споткнётся
        double gain = 0.0;
        bool ok = iio_channel_attr_read_double(chn, "hardwaregain", &gain) == 0;
        if (ok) gain_out.store(static_cast<float>(gain));

        // rssi — строка вида "12.25 dB", числового аксессора у неё нет
        char rssi_str[64] = { 0 };
        if (iio_channel_attr_read(chn, "rssi", rssi_str, sizeof(rssi_str)) > 0) {
            char* endp = nullptr;
            float rssi = strtof(rssi_str, &endp);
            if (endp != rssi_str) rssi_out.store(rssi);
            // иначе плата вернула что-то нечисловое — прошлое значение честнее нуля
        }
        return ok;
    };

    bool ok = read_pair(phy_chn0_, state_.gain_actual_db, state_.rssi_db);
    if (current_mode_ == PlutoChannelMode::Channel_2 || current_mode_ == PlutoChannelMode::Both)
        read_pair(phy_chn1_, state_.gain_actual_db2, state_.rssi_db2);
    state_.gain_readback_valid.store(ok);
}

void PlutoCaptureThread::close_device() {
    if (rx_buf_) {
        iio_buffer_destroy(rx_buf_);
        rx_buf_ = nullptr;
    }
    if (rx_i0_) iio_channel_disable(rx_i0_);
    if (rx_q0_) iio_channel_disable(rx_q0_);
    if (rx_i1_) iio_channel_disable(rx_i1_);
    if (rx_q1_) iio_channel_disable(rx_q1_);

    rx_i0_ = rx_q0_ = rx_i1_ = rx_q1_ = nullptr;
    phy_chn0_ = phy_chn1_ = nullptr;
    state_.gain_readback_valid.store(false);
    state_.cross_phase_valid.store(false);
    // Про инверсию без связи мы ничего не знаем: и «не поддерживается», и
    // «применено» одинаково соврали бы про отключённую плату.
    state_.rx_phase_inversion_supported.store(false);
    state_.rx_phase_inversion_applied.store(false);

    if (ctx_) {
        iio_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    phy_dev_ = rx_dev_ = nullptr;
}

// Перечитывает таблицу с диска. Вызывается при старте потока и когда UI
// просит перезагрузить: править CSV и видеть результат без перезапуска
// приложения — обычный сценарий, пока калибровка ещё уточняется.
void PlutoCaptureThread::reload_calibration() {
    std::string path, error;
    {
        lock_guard<mutex> lock(state_.rx_cal_mutex);
        path = state_.rx_cal_path;
    }

    bool ok = calibration_.load(path, error);
    state_.rx_cal_loaded.store(ok);
    if (ok) state_.rx_cal_generation.fetch_add(1); // поток пеленгации перечитает свою копию
    {
        lock_guard<mutex> lock(state_.rx_cal_mutex);
        state_.rx_cal_status = ok ? calibration_.source() : error;
    }
    cout << (ok ? "[pluto] calibration loaded: " : "[pluto] calibration not loaded: ")
         << (ok ? calibration_.source() : error) << "\n";

    // Таблица без данных не должна оставлять галочку включённой: иначе UI
    // показывает применённую калибровку, а к отсчётам не применяется ничего.
    if (!ok) state_.rx_cal_enabled.store(false);
}

// Множитель для Rx2 на текущих частоте и усилении. Считается раз в буфер.
void PlutoCaptureThread::update_calibration_correction() {
    bool want = state_.rx_cal_enabled.load() && calibration_.valid()
                && current_mode_ == PlutoChannelMode::Both;

    if (!want) {
        cal_active_ = false;
        cal_correction_ = { 1.0f, 0.0f };
        state_.rx_cal_in_range.store(false);
        return;
    }

    double freq = static_cast<double>(current_freq_);
    double gain = static_cast<double>(current_gain_db_);

    float phase_deg = 0.0f, amp_db = 0.0f;
    calibration_.imbalance(freq, gain, phase_deg, amp_db);
    cal_correction_ = calibration_.correction(freq, gain);
    cal_active_     = true;

    state_.rx_cal_phase_deg.store(phase_deg);
    state_.rx_cal_amp_db.store(amp_db);
    // За краем сетки поправка берётся с границы: она всё ещё лучше нуля, но
    // это уже не измерение, и UI должен сказать об этом честно.
    state_.rx_cal_in_range.store(calibration_.in_range(freq, gain));
}

// Чистый доп. поворот Rx2 (magnitude 1) — знак такой же, как у RxCalibration::
// correction(): положительный trim прибавляется прямо к cross_phase_deg,
// поэтому чтобы свести показание в 0, в поле вводится текущее показание с
// обратным знаком.
void PlutoCaptureThread::update_manual_trim() {
    float trim_deg = state_.manual_phase_trim_deg.load();
    manual_trim_active_ = (trim_deg != 0.0f);
    float rad = -trim_deg * static_cast<float>(PI) / 180.0f;
    manual_trim_correction_ = { std::cos(rad), std::sin(rad) };
}

void PlutoCaptureThread::process_rx_buffer() {
    update_calibration_correction();
    update_manual_trim();

    ptrdiff_t step = iio_buffer_step(rx_buf_);
    char* end_ptr = static_cast<char*>(iio_buffer_end(rx_buf_));
    
    // В зависимости от активного режима выбираем ведущий канал для выравнивания указателя
    struct iio_channel* lead_chan = (current_mode_ == PlutoChannelMode::Channel_2) ? rx_i1_ : rx_i0_;
    char* src_ptr = static_cast<char*>(iio_buffer_first(rx_buf_, lead_chan));

    // Взаимная корреляция Rx1/Rx2 по буферу: фаза показывает, насколько
    // разъезжаются тракты (у AD9361 это штатные 180°, если не включена инверсия
    // Rx2), а когерентность — можно ли этой фазе верить. Копится по тем же
    // отсчётам, что и так разбираются, отдельного прохода по памяти нет.
    double cross_re = 0.0, cross_im = 0.0, power1 = 0.0, power2 = 0.0;
    size_t cross_n = 0;

    for (; src_ptr < end_ptr; src_ptr += step) {
        int16_t* words = reinterpret_cast<int16_t*>(src_ptr);
        CoherentSample sample;

        // Только Both включает все 4 канала (i1,q1,i2,q2) — step тогда 8 байт.
        // В Channel_1/Channel_2 включены лишь 2 канала (step 4 байта), поэтому нельзя
        // безусловно читать 4 int16: это читало бы за пределы текущего сэмпла (в
        // следующий) и в режиме Channel_2 записывало бы реальные данные Rx2 в поле rx1.
        if (current_mode_ == PlutoChannelMode::Both) {
            // Перегрузка считается здесь, по сырым кодам АЦП: ниже отсчёт
            // делится на шкалу, а Rx2 ещё и поворачивается калибровкой, и
            // «упёрся в шкалу» после этого уже не отличить от «просто громкий».
            clip_monitor_.add(words[0], words[1], words[2], words[3]);

            // Нормализация к float (12-бит АЦП AD9361)
            sample.rx1 = std::complex<float>(static_cast<float>(words[0]) / 2048.0f, static_cast<float>(words[1]) / 2048.0f);
            sample.rx2 = std::complex<float>(static_cast<float>(words[2]) / 2048.0f, static_cast<float>(words[3]) / 2048.0f);

            // Калибровка правит Rx2 здесь, до всего остального: и кросс-фаза
            // ниже, и пеленг, и спектр берут уже исправленные отсчёты, так что
            // поправку не нужно повторять (и забывать) в каждом потребителе.
            if (cal_active_) sample.rx2 *= cal_correction_;
            if (manual_trim_active_) sample.rx2 *= manual_trim_correction_;

            // rx1 * conj(rx2), накопление по буферу
            cross_re += static_cast<double>(sample.rx1.real()) * sample.rx2.real()
                      + static_cast<double>(sample.rx1.imag()) * sample.rx2.imag();
            cross_im += static_cast<double>(sample.rx1.imag()) * sample.rx2.real()
                      - static_cast<double>(sample.rx1.real()) * sample.rx2.imag();
            power1   += static_cast<double>(std::norm(sample.rx1));
            power2   += static_cast<double>(std::norm(sample.rx2));
            ++cross_n;
        } else if (current_mode_ == PlutoChannelMode::Channel_2) {
            clip_monitor_.add(words[0], words[1]);
            sample.rx2 = std::complex<float>(static_cast<float>(words[0]) / 2048.0f, static_cast<float>(words[1]) / 2048.0f);
        } else {
            clip_monitor_.add(words[0], words[1]);
            sample.rx1 = std::complex<float>(static_cast<float>(words[0]) / 2048.0f, static_cast<float>(words[1]) / 2048.0f);
        }

        fft_buf_.push(sample);
        df_buf_.push(sample);
        demod_buf_.push(sample);
    }

    if (current_mode_ == PlutoChannelMode::Both && power1 > 0.0 && power2 > 0.0 && cross_n > 0) {
        double magnitude = sqrt(cross_re * cross_re + cross_im * cross_im);
        // Проверка: деление суммы на N перед atan2 не меняет arg(), значение должно остаться прежним.
        double avg_re = cross_re;
        double avg_im = cross_im;
        // PI из app_state.hpp, а не M_PI: последний на MSVC живёт под
        // _USE_MATH_DEFINES, а сборка под Windows здесь тоже есть.
        state_.cross_phase_deg.store(static_cast<float>(atan2(avg_im, avg_re) * 180.0 / PI));
        state_.cross_coherence.store(static_cast<float>(magnitude / sqrt(power1 * power2)));
        state_.cross_phase_valid.store(true);

        // Время — по числу отсчётов, не по часам: та же логика, что у
        // clip_monitor_ (см. его комментарий), и по той же причине —
        // воспроизведение файла идёт не строго в темпе реального времени.
        phase_stream_time_s_ += static_cast<double>(cross_n) / std::max<double>(current_sample_rate_, 1.0);
        {
            lock_guard<mutex> lock(state_.phase_hist_mutex);
            state_.phase_hist_time_s.push_back(static_cast<float>(phase_stream_time_s_));
            state_.phase_hist_deg.push_back(state_.cross_phase_deg.load());
            if (static_cast<int>(state_.phase_hist_time_s.size()) > AppState::PHASE_HISTORY_LEN) {
                state_.phase_hist_time_s.erase(state_.phase_hist_time_s.begin());
                state_.phase_hist_deg.erase(state_.phase_hist_deg.begin());
            }
        }
    } else {
        state_.cross_phase_valid.store(false);
    }
}

void PlutoCaptureThread::log_error(const string& context, int error_code) {
    char err_str[256];
    iio_strerror(-error_code, err_str, sizeof(err_str));
    cerr << "[pluto] " << context << " failed: " << err_str << "\n";
}

void PlutoCaptureThread::save_iq_file() {
    filesystem::path data_dir = executable_dir() / "data";
    filesystem::create_directories(data_dir);
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));

    // Число каналов попадает в имя файла: сам файл его не несёт, а разобрать
    // двухканальную запись как одноканальную — значит принять отсчёты Rx2 за
    // следующие по времени отсчёты Rx1. По этому суффиксу воспроизведение
    // подставляет себе раскладку. Имена без суффикса — одноканальные, как и
    // все записи, сделанные до его появления.
    bool   dual   = state_.chan_mode.load() == PlutoChannelMode::Both;
    string suffix = dual ? "_2ch" : "";

    string name;
    {
        lock_guard<mutex> lock(state_.save_mutex);
        name = state_.save_file_name;
    }
    if (name.empty()) {
        name = string("iq_pluto_") + ts + "_" + to_string(state_.center_freq_hz.load() / 1'000'000) + "MHz";
    }
    // Суффикс и расширение дописываем сами: по ним воспроизведение узнаёт
    // раскладку файла, а имя приходит из поля ввода, где про них не помнят.
    // Расширение сначала снимаем, чтобы суффикс встал перед ним, а не после;
    // уже проставленный вручную суффикс не дублируем.
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".bin") == 0)
        name.erase(name.size() - 4);
    if (dual && name.find("_2ch") == string::npos) name += suffix;
    name += ".bin";

    filesystem::path path = data_dir / name;
    filesystem::create_directories(path.parent_path()); // имя может содержать подкаталог
    ofstream f(path, ios::binary);
    if (!f) return;

    state_.is_saving.store(true);
    state_.save_dropped_samples.store(0);
    state_.save_clip_valid.store(false);

    // Диск не всегда успевает за потоком с Pluto (особенно на высоком sample rate
    // или в режиме Both). Запись на диск вынесена в отдельный поток с ограниченной
    // очередью, чтобы временное подтормаживание диска не задерживало
    // iio_buffer_refill() и не приводило к переполнению DMA-буфера устройства.
    constexpr size_t kMaxQueuedBytes = 256u * 1024 * 1024; // ~256MB запаса на подтормаживание диска

    mutex               queue_mutex;
    condition_variable  queue_cv;
    deque<vector<char>> queue;
    size_t              queued_bytes  = 0;
    bool                producer_done = false;
    bool                write_failed  = false;

    thread writer([&] {
        unique_lock<mutex> lock(queue_mutex);
        for (;;) {
            queue_cv.wait(lock, [&] { return !queue.empty() || producer_done; });
            while (!queue.empty()) {
                vector<char> chunk = std::move(queue.front());
                queue.pop_front();
                queued_bytes -= chunk.size();
                lock.unlock();
                f.write(chunk.data(), static_cast<streamsize>(chunk.size()));
                if (!f) write_failed = true;
                lock.lock();
            }
            if (producer_done && queue.empty()) break;
        }
    });

    int log2n = clamp(state_.save_length_log2.load(), AppState::SAVE_LOG2_MIN, AppState::SAVE_LOG2_MAX);

    unsigned int total   = 1u << log2n;
    unsigned int written = 0;
    uint64_t     dropped = 0;
    // Перегрузка в самой записи. Считается по тем же байтам, что уходят в файл,
    // тем же порогом, что и на живом потоке (см. include/clip_monitor.hpp): о
    // зажатых отсчётах надо знать сразу, пока стенд ещё собран и усиление
    // можно убавить, а не от того, кто через неделю смотрит эту запись.
    ClipScan clip_scan;

    while (written < total && state_.running.load() && !write_failed) {
        ssize_t n_bytes = iio_buffer_refill(rx_buf_);
        if (n_bytes < 0) {
            log_error("iio_buffer_refill (recording)", n_bytes);
            break;
        }

        struct iio_channel* lead_chan = (state_.chan_mode.load() == PlutoChannelMode::Channel_2) ? rx_i1_ : rx_i0_;
        char* src_ptr = static_cast<char*>(iio_buffer_first(rx_buf_, lead_chan));
        ptrdiff_t step = iio_buffer_step(rx_buf_);
        size_t samples_in_buffer = (static_cast<char*>(iio_buffer_end(rx_buf_)) - src_ptr) / step;
        // Последний буфер обрезаем по границе запрошенной длины: она задана
        // степенью двойки именно затем, чтобы файл содержал целое число окон
        // БПФ, а размер буфера libiio с ней не связан и почти всегда её
        // перешагнёт. Хвост отбрасывается, а не дописывается.
        samples_in_buffer = min<size_t>(samples_in_buffer, total - written);
        size_t bytes = samples_in_buffer * step;

        clip_scan.scan(reinterpret_cast<const int16_t*>(src_ptr), samples_in_buffer,
                        static_cast<size_t>(step) / sizeof(int16_t));

        written += samples_in_buffer;

        lock_guard<mutex> lock(queue_mutex);
        if (queued_bytes + bytes > kMaxQueuedBytes) {
            // Писатель отстаёт настолько, что мы уже выбрали весь запас очереди —
            // честно фиксируем разрыв вместо того, чтобы блокировать refill() и
            // молча терять данные на стороне устройства.
            dropped += samples_in_buffer;
        } else {
            queue.emplace_back(src_ptr, src_ptr + bytes);
            queued_bytes += bytes;
            queue_cv.notify_one();
        }
    }

    {
        lock_guard<mutex> lock(queue_mutex);
        producer_done = true;
    }
    queue_cv.notify_one();
    writer.join();
    f.close();

    state_.save_dropped_samples.store(dropped);
    state_.save_clip_pct.store(clip_scan.clipped_pct());
    state_.save_peak_dbfs.store(clip_scan.peak_dbfs());
    state_.save_clip_valid.store(clip_scan.total > 0);
    state_.is_saving.store(false);

    if (clip_scan.clipped > 0) {
        cerr << "[pluto] IQ recording: " << clip_scan.clipped << " of " << clip_scan.total
             << " samples clipped (" << clip_scan.clipped_pct() << "%), peak "
             << clip_scan.peak_dbfs() << " dBFS - reduce gain\n";
    }

    if (dropped > 0) {
        cerr << "[pluto] IQ recording: dropped " << dropped << " samples (~"
             << (dropped * 1000ULL / max<uint64_t>(state_.sample_rate_hz.load(), 1)) << " ms) - disk could not keep up\n";
    }
    if (write_failed) {
        cerr << "[pluto] IQ recording: write error, file may be truncated\n";
    }
    cout << "[pluto] IQ recording saved: " << path << " (" << written << "/" << total << " samples)\n";
}

// Полный снимок SPI-регистров чипа, а не именованных debug-атрибутов вроде
// initialize/adi,... — те покрывают только то, что оформлено параметром
// платформы, а тут нужен весь набор, включая недокументированные адреса.
// Диапазон 0x000..0x3FF взят по верхнему определению в ad9361_regs.h прошивки
// платы (ad9361_phy_reg_access() в драйвере читает адрес без проверки границ,
// так что значения за пределами реально занятых регистров тоже приходят —
// они просто не несут смысла).
void PlutoCaptureThread::dump_ad9361_registers() {
    if (!state_.is_connected.load() || !phy_dev_) {
        lock_guard<mutex> lock(state_.ad9361_reg_dump_mutex);
        state_.ad9361_reg_dump_status = "not connected";
        return;
    }

    state_.ad9361_reg_dump_in_progress.store(true);

    filesystem::path data_dir = executable_dir() / "data";
    filesystem::create_directories(data_dir);
    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&now));
    filesystem::path path = data_dir / (string("ad9361_regs_") + ts + ".csv");

    ofstream f(path);
    if (!f) {
        lock_guard<mutex> lock(state_.ad9361_reg_dump_mutex);
        state_.ad9361_reg_dump_status = "could not open " + path.string();
        state_.ad9361_reg_dump_in_progress.store(false);
        return;
    }

    static constexpr uint32_t kRegMax = 0x3FF;
    f << "address,value\n";
    int ok_count = 0, err_count = 0;
    for (uint32_t addr = 0; addr <= kRegMax; ++addr) {
        uint32_t value = 0;
        int ret = iio_device_reg_read(phy_dev_, addr, &value);
        char line[32];
        if (ret == 0) {
            snprintf(line, sizeof(line), "0x%03X,0x%02X\n", addr, value);
            ++ok_count;
        } else {
            snprintf(line, sizeof(line), "0x%03X,ERROR\n", addr);
            ++err_count;
        }
        f << line;
    }
    f.close();

    string status = path.string() + " (" + to_string(ok_count) + " ok";
    if (err_count > 0) status += ", " + to_string(err_count) + " errors";
    status += ")";
    {
        lock_guard<mutex> lock(state_.ad9361_reg_dump_mutex);
        state_.ad9361_reg_dump_status = status;
    }
    cout << "[pluto] AD9361 register dump saved: " << status << "\n";
    state_.ad9361_reg_dump_in_progress.store(false);
}
