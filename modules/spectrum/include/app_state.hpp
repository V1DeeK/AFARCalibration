#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <string>
#include <complex>

static constexpr float PI = 3.14159265358979323846f;
using Complex = std::complex<float>;

enum class PlutoChannelMode {
    Channel_1,
    Channel_2,
    Both
};

// Управление усилением приёмного тракта AD9361. Чип не выставляет каскады
// (LNA/микшер/TIA/ФНЧ) по отдельности: один индекс в таблице усиления задаёт
// готовую их комбинацию, а драйвер пересчитывает в этот индекс запрошенные дБ.
// Кто выбирает индекс — и есть режим:
//   Manual     — индекс держим мы, усиление не меняется само (измерения, калибровка)
//   SlowAttack — АРУ по усреднённой мощности, шагает медленно (непрерывный сигнал)
//   FastAttack — АРУ по пиковым детекторам, сбрасывает усиление внутри пачки
//   Hybrid     — индекс наш, но детекторы перегрузки сами его уменьшают
enum class GainMode {
    Manual,
    SlowAttack,
    FastAttack,
    Hybrid
};

enum class DemodMode {
    Off,
    FmAudio,
    TvVideo
};

enum class TvStandard {
    NTSC,
    PAL
};

struct AppState {
    // connection setting
    std::mutex            connection_mutex;
    std::string           pluto_ip        { "ip:192.168.2.1" }; 
    std::atomic<uint32_t> sample_rate_hz  { 20'000'000 };
    std::atomic<uint32_t> bandwidth_hz   { 20'000'000 };
    std::atomic<int>      rf_port_idx     { 0 }; // 0 = A_BALANCED, 1 = B_BALANCED, etc.
    std::atomic<PlutoChannelMode> chan_mode { PlutoChannelMode::Channel_1 };


    std::mutex            uri_list_mutex;
    std::vector<std::string> found_uris        { "local" }; 
    std::atomic<bool>     scan_requested    { false };
    std::atomic<bool>     is_scanning       { false };

    std::atomic<bool>     connect_requested{ false };
    // Разрыв связи по кнопке. Одноразовый флаг: UI ставит, CaptureThread
    // закрывает контекст libiio и сбрасывает обратно.
    std::atomic<bool>     disconnect_requested{ false };
    std::atomic<bool>     is_connected     { false };

    // Поиск платы по MAC — UI пишет запрос, BoardFinderThread выполняет.
    // Адрес плате выдаёт DHCP и он меняется между загрузками, а MAC постоянный,
    // поэтому искать по нему надёжнее, чем помнить последний увиденный IP.
    std::mutex            board_mutex;
    std::string           board_mac       { "42:7e:d7:4e:0f:77" };
    std::string           board_found_ip;  // пусто, пока не найдена
    std::string           board_status;    // пояснение для UI
    std::atomic<bool>     board_find_requested{ false };
    std::atomic<bool>     board_is_finding    { false };

    // config — UI write, capture + processing read
    std::atomic<uint64_t> center_freq_hz  { 5'800'000'000ULL };
    std::atomic<int>      fft_size        { 4096 };
    std::atomic<float>    cal_offset_db   { 0.0f };
    std::atomic<bool>     peak_hold_on    { false };
    std::atomic<bool>     peak_hold_reset { false };
    // Центральный бин (DC) на приёмнике прямого преобразования всегда завышен
    // паразитным пролезанием гетеродина, а не сигналом в эфире. По умолчанию
    // ProcessingThread::convert_to_dbm() подменяет его интерполяцией соседей;
    // галочка позволяет увидеть бин как есть, не трогая остальной спектр.
    std::atomic<bool>     exclude_dc_bin  { true };

    // Усиление Rx — UI пишет, CaptureThread читает и применяет на лету, без
    // переподключения: это обычные атрибуты канала, а не параметры потока вроде
    // частоты дискретизации, ради которых надо пересоздавать буфер.
    // gain_db действует только в GainMode::Manual и Hybrid — в режимах АРУ
    // индекс усиления выбирает сам чип, и запись hardwaregain драйвер отклоняет.
    // Границы — диапазон, который принимает драйвер ad9361-phy на нижнем
    // диапазоне; выше 1300 МГц таблица усиления короче, и запрос сверх её
    // максимума не пройдёт (видно по обратной связи gain_actual_db ниже).
    static constexpr int  GAIN_DB_MIN = -3;
    static constexpr int  GAIN_DB_MAX = 71;
    std::atomic<GainMode> gain_mode       { GainMode::SlowAttack };
    std::atomic<int>      gain_db         { 30 }; // Rx1
    std::atomic<int>      gain_db2        { 30 }; // Rx2
    // Rx2 повторяет уровень Rx1. В двухканальном режиме тракты сравниваются по
    // амплитуде (пеленг, когерентность), и разное усиление на них — это
    // систематическая ошибка измерения, а не просто разная громкость каналов.
    std::atomic<bool>     gain_link_channels{ true };

    // Инверсия Rx2 (adi,rx1-rx2-phase-inversion-enable у ad9361-phy). У AD9361
    // второй приёмник по умолчанию идёт в противофазе первому: измерено на плате
    // 180° ± 3° между каналами на 2400..2430 МГц, с включённой настройкой —
    // единицы градусов. Для пеленга это не мелкая поправка, а зеркальное
    // направление, поэтому по умолчанию включено.
    // Настройка живёт в platform data и подхватывается только переинициализацией
    // чипа, так что применяется переподключением, как и всё в секции Device.
    std::atomic<bool>     rx_phase_inversion { true };
    // Плата ответила на debug-атрибут (есть не во всех прошивках) и запись прошла.
    std::atomic<bool>     rx_phase_inversion_supported{ false };
    std::atomic<bool>     rx_phase_inversion_applied  { false };

    // Стабильная фаза: отключение адаптивных алгоритмов AD9361 для измерения
    // фазы между каналами без AM-to-PM эффекта. При включении отключаются:
    // - rf_dc_offset_tracking_en
    // - bb_dc_offset_tracking_en
    // - quadrature_tracking_en
    std::atomic<bool>     phase_stable_mode { true };

    // Калибровка перекоса Rx2/Rx1 из data/rx_calibration.csv. Таблица снята
    // генератором по сетке частота × усиление и содержит перекос самого чипа:
    // кабели стенда из неё вычтены прогоном с их перестановкой. Применяется к
    // отсчётам Rx2 сразу при разборе буфера, то есть до кросс-корреляции,
    // пеленга и спектра — одна точка на всё, а не поправка в каждом потребителе.
    std::atomic<bool>     rx_cal_enabled { false };
    std::atomic<bool>     rx_cal_loaded  { false };
    std::atomic<bool>     rx_cal_reload_requested { false };
    // Счётчик версий таблицы: поток захвата увеличивает его после каждой
    // успешной загрузки, поток пеленгации по нему понимает, что пора перечитать
    // свою копию. Одноразовый флаг выше для этого не годится — его забирает
    // exchange() тот, кто успел первым.
    std::atomic<uint32_t> rx_cal_generation { 0 };
    // Поправка, действующая на текущей частоте и усилении, — в UI, чтобы было
    // видно, что именно вычитается, а не только что галочка нажата.
    std::atomic<float>    rx_cal_phase_deg { 0.0f };
    std::atomic<float>    rx_cal_amp_db    { 0.0f };
    std::atomic<bool>     rx_cal_in_range  { false };  // точка внутри сетки таблицы
    std::string           rx_cal_path   { "data/rx_calibration.csv" };
    std::string           rx_cal_status;               // сообщение загрузки для UI
    std::mutex            rx_cal_mutex;                // защищает две строки выше

    // Разность фаз Rx1/Rx2 и когерентность по последнему буферу — CaptureThread
    // пишет, UI читает. Считается по всей полосе сразу, поэтому фаза осмысленна
    // только при высокой когерентности: на шуме это равномерно случайное число,
    // и показывать его как измерение нельзя.
    std::atomic<bool>     cross_phase_valid { false };
    std::atomic<float>    cross_phase_deg   { 0.0f };
    std::atomic<float>    cross_coherence   { 0.0f };

    // Ручная подстройка фазы Rx2 поверх calibration_: независимый доп. поворот,
    // не привязанный к таблице и никуда не сохраняемый — покрутить, глядя на
    // график истории ниже. UI пишет, CaptureThread читает раз в буфер.
    std::atomic<float>    manual_phase_trim_deg { 0.0f };

    // История cross_phase_deg во времени — та же идея, что у clip_hist_* ниже:
    // время от начала потока (по числу отсчётов, не по часам) и значение,
    // старое в начале. Пишется раз в буфер: он и так фиксированной длины
    // (fs/50), отдельное окно усреднения не нужно.
    static constexpr int  PHASE_HISTORY_LEN = 1500;  // ~30 с при буфере fs/50
    std::mutex             phase_hist_mutex;
    std::vector<float>     phase_hist_time_s;
    std::vector<float>     phase_hist_deg;

    // Обратная связь от платы — CaptureThread пишет, UI читает. В режимах АРУ
    // это единственный способ увидеть, на чём усиление остановилось; в ручном —
    // проверка, что запрошенные дБ вообще прошли (см. про таблицу усиления выше).
    std::atomic<bool>     gain_readback_valid{ false };
    std::atomic<float>    gain_actual_db  { 0.0f };
    std::atomic<float>    gain_actual_db2 { 0.0f };
    std::atomic<float>    rssi_db         { 0.0f }; // уровень на входе, дБ (0 = нет данных)
    std::atomic<float>    rssi_db2        { 0.0f };

    std::atomic<bool>     running         { true };
    std::atomic<int>      total_frames    { 0 };
    std::atomic<bool>     save_requested  { false };
    std::atomic<bool>     is_saving          { false };
    std::atomic<uint64_t> save_dropped_samples{ 0 }; // samples lost in the last recording because disk writes fell behind capture

    // Длина записи IQ — степень двойки, 2^16..2^20 отсчётов. Файл потом
    // разбирают окнами БПФ (см. python/psd_generate.py, IqPlaybackThread), и
    // объём, кратный степени двойки, — это целое число окон без огрызка в
    // конце. Хранится показателем, а не самим числом: так UI не может
    // предложить промежуточное значение, а запись — записать некратную длину.
    static constexpr int  SAVE_LOG2_MIN = 16; //   65536 отсчётов
    static constexpr int  SAVE_LOG2_MAX = 20; // 1048576 отсчётов
    std::atomic<int>      save_length_log2{ SAVE_LOG2_MAX };

    // Имя файла записи IQ. Пустое — собрать автоматически из времени и частоты,
    // как было до появления настройки. Кладётся в data/ рядом с исполняемым
    // файлом; суффикс _2ch и расширение .bin дописывает save_iq_file(), потому
    // что от них зависит разбор файла при воспроизведении.
    std::mutex            save_mutex;
    std::string           save_file_name{ "" };

    // Выгрузка всех регистров AD9361 (0x000..0x3FF, прямой SPI-доступ через
    // iio_device_reg_read, а не именованные debug-атрибуты) в CSV. UI пишет
    // запрос, CaptureThread — единственный владелец phy_dev_ — читает регистры
    // и кладёт файл рядом с записями IQ.
    std::mutex             ad9361_reg_dump_mutex;
    std::string            ad9361_reg_dump_status; // путь к файлу или текст ошибки
    std::atomic<bool>      ad9361_reg_dump_requested  { false };
    std::atomic<bool>      ad9361_reg_dump_in_progress{ false };


    // peleng (direction finding) output — subband monopulse statistics,
    // beam fixed at boresight (0 deg) in this first version.
    // DF thread writes, display thread reads.
    std::mutex            peleng_mutex;
    std::vector<float>    peleng_freq_mhz;   // subband center frequency, absolute MHz
    std::vector<float>    peleng_rho_re;     // Re{Delta/Sigma}
    std::vector<float>    peleng_rho_im;     // Im{Delta/Sigma}
    std::vector<float>    peleng_gamma2;     // RX1/RX2 coherence, 0..1
    std::vector<float>    peleng_snr_db;
    std::vector<float>    peleng_sigma_dbfs;
    std::vector<uint8_t>  peleng_valid;      // quality mask q_p (1 = usable subband)
    std::atomic<bool>     peleng_has_data    { false };
    // Калибровка каналов C1[p]/C2[p] в пеленгаторе: применена ли она к текущим
    // блокам и с какой задержкой. Пишет DirectionFindingThread, читает UI.
    std::atomic<bool>     peleng_cal_active  { false };
    std::atomic<float>    peleng_cal_delay_ns{ 0.0f };
    std::atomic<uint64_t> peleng_update_count{ 0 };

    // peleng source — UI writes, DirectionFindingThread reads. In Network mode
    // the subband statistics come already accumulated from the Zynq PL over TCP
    // (see include/network_stats.hpp) instead of being computed here from the
    // local IQ stream, so no SDR connection is needed at all.
    std::atomic<bool>     peleng_use_network { false };
    // When set, DirectionFindingThread runs BOTH the local and network paths
    // every iteration regardless of peleng_use_network (which then only picks
    // which one drives the live graph/AppState fields below) and tags every
    // logged CSV row with its source, so a recording can compare the PC's own
    // model against what the board's PL actually computed.
    std::atomic<bool>     peleng_compare_mode{ false };
    std::atomic<uint16_t> peleng_net_port    { 5310 };
    std::atomic<bool>     peleng_net_connected{ false }; // DF thread writes, display reads (status text)
    // FFT/subband geometry of the last received block — reported by the board
    // in Network mode, taken from SubbandAccumulator::Params otherwise
    std::atomic<int>      peleng_fft_size    { 4096 };
    std::atomic<int>      peleng_subband_size{ 16 };
    std::atomic<int>      peleng_accum_frames{ 128 };

    // peleng CSV logging — UI writes the path and the one-shot request flags,
    // DirectionFindingThread owns the file. Пишет именно поток пеленгации, а не
    // отображения: блоки статистик приходят со своим темпом (N_FFT*M отсчётов
    // на блок), и снимок, который успевает взять кадр отрисовки, — это лишь
    // последний блок, а в журнале нужны все подряд, без пропусков.
    std::mutex             peleng_log_mutex;
    std::string            peleng_log_path{ "data/peleng_log.csv" };
    std::atomic<bool>      peleng_log_start_requested{ false }; // one-shot: открыть файл и начать писать
    std::atomic<bool>      peleng_log_stop_requested { false }; // one-shot: закрыть файл
    std::atomic<bool>      peleng_is_logging         { false };
    std::atomic<bool>      peleng_log_error          { false }; // файл не открылся или запись сорвалась
    std::atomic<uint64_t>  peleng_log_rows_written   { 0 };
    std::atomic<uint64_t>  peleng_log_blocks_written { 0 };
    // false = полный срез спектра, все подполосы на каждый блок;
    // true  = только выбранная в UI подполоса. Полный срез — это n строк на
    // блок при десятках блоков в секунду, поэтому при изучении одной частоты
    // файл разумно сузить до неё одной.
    std::atomic<bool>      peleng_log_selected_only  { false };

    // подполоса, выбранная в окне пеленга для графика rho на комплексной
    // плоскости. UI пишет, поток пеленгации читает (режим журнала выше).
    // Индекс — уже после fftshift, то есть тот же, что у peleng_freq_mhz и
    // остальных векторов выше. -1 = ничего не выбрано.
    std::atomic<int>       peleng_selected_subband   { -1 };

    // psd output — processing writes, display reads
    // ch2 fields are only populated when chan_mode == Both (dual-channel view)
    std::mutex            psd_mutex;
    std::vector<float>    psd_dbm;
    std::vector<float>    peak_dbm;
    std::vector<float>    psd_dbm2;
    std::vector<float>    peak_dbm2;

    static constexpr int  WATERFALL_ROWS = 64;
    std::vector<float>    waterfall;
    std::vector<float>    waterfall2;

    // ---- Отношение сигнал/шум по спектру ----
    // ProcessingThread пишет по каждому опубликованному спектру, UI читает.
    // Шумовой пол — медиана по бинам: она держится за шум, пока сигналы
    // занимают меньше половины полосы, тогда как среднее уводит вверх любой
    // сильный несущей. Сигнал — самый сильный бин.
    // SNR здесь — «пик над шумовым полом в одном бине», как его показывает
    // анализатор спектра, а не SNR демодулятора в полосе канала: ширину
    // сигнала спектру неоткуда знать, и она у каждого сигнала своя.
    std::atomic<bool>     snr_valid     { false };
    std::atomic<float>    snr_db        { 0.0f };
    std::atomic<float>    snr_noise_dbm { 0.0f };
    std::atomic<float>    snr_peak_dbm  { 0.0f };
    std::atomic<double>   snr_peak_hz   { 0.0 };
    std::atomic<bool>     snr_valid2    { false };  // второй канал, только в режиме Both
    std::atomic<float>    snr_db2       { 0.0f };
    std::atomic<float>    snr_noise_dbm2{ 0.0f };
    std::atomic<float>    snr_peak_dbm2 { 0.0f };
    std::atomic<double>   snr_peak_hz2  { 0.0 };
    // Рисовать ли на спектре линию шумового пола и подпись у пика. Числа те же,
    // что в строке под графиком, — галочка только про разметку на самом графике.
    std::atomic<bool>     snr_overlay_on{ true };

    // ---- Перегрузка входа (клиппинг) ----
    // Пишет тот поток, который сейчас гонит отсчёты — захват с платы или
    // воспроизведение файла (см. include/clip_monitor.hpp), читает UI.
    // Нужно и на живом сигнале, и при просмотре записи: зажатые отсчёты рвут
    // и спектр, и демодуляцию, а по картинке спектра это не опознать —
    // перегрузка выглядит как «богатый» сигнал с гребёнкой гармоник, то есть
    // как что-то, что в эфире и правда бывает.
    static constexpr int  CLIP_HISTORY_LEN = 300;  // ~30 с при окне 100 мс
    std::atomic<bool>     clip_stream_active { false }; // отсчёты идут, показаниям есть чему верить
    std::atomic<bool>     clip_active        { false }; // в последнем окне были зажатые отсчёты
    std::atomic<float>    clip_pct1          { 0.0f };  // доля зажатых отсчётов в последнем окне, %
    std::atomic<float>    clip_pct2          { 0.0f };
    std::atomic<float>    clip_peak_dbfs1    { -120.0f }; // пик за окно относительно шкалы АЦП
    std::atomic<float>    clip_peak_dbfs2    { -120.0f };
    std::atomic<float>    clip_worst_pct     { 0.0f };  // худшее окно с начала потока
    std::atomic<uint64_t> clip_windows       { 0 };     // окон с клиппингом
    std::atomic<uint64_t> clip_windows_total { 0 };     // окон всего — чтобы «часто» было числом
    std::atomic<double>   clip_last_time_s   { -1.0 };  // когда клиппинг был последний раз
    std::atomic<double>   clip_stream_time_s { 0.0 };   // сколько отсчётов прошло, в секундах потока
    std::atomic<bool>     clip_reset_requested{ false };// UI: обнулить счётчики и историю
    // История окон для полосы под водопадом: время от начала потока, доля
    // зажатых отсчётов и пиковый уровень по каждому каналу. Старое в начале.
    std::mutex            clip_mutex;
    std::vector<float>    clip_hist_time_s;
    std::vector<float>    clip_hist_pct1;
    std::vector<float>    clip_hist_pct2;
    std::vector<float>    clip_hist_dbfs1;
    std::vector<float>    clip_hist_dbfs2;

    // Итог по последней записи IQ: считается по тем же сырым отсчётам, что
    // уходят в файл, — чтобы про перегрузку было известно сразу после записи,
    // а не от того, кто потом эту запись смотрел.
    std::atomic<bool>     save_clip_valid { false };
    std::atomic<float>    save_clip_pct   { 0.0f };
    std::atomic<float>    save_peak_dbfs  { -120.0f };

    // demod config — UI writes, DemodThread + AudioOutputDevice read
    std::atomic<DemodMode> demod_mode        { DemodMode::Off };
    std::atomic<uint32_t>  fm_channel_bw_hz  { 200'000 };
    std::atomic<bool>      fm_deemphasis_on  { true };
    std::atomic<float>     audio_volume      { 0.8f };
    std::atomic<bool>      audio_mute        { false };
    std::atomic<bool>      audio_device_ok   { false }; // AudioOutputDevice writes, display reads (status text)

    // FM audio demod output — DemodThread writes, display reads
    std::mutex             demod_audio_mutex;
    std::vector<float>     demod_audio_waveform; // short recent snapshot for the waveform plot
    std::atomic<bool>      demod_audio_has_data    { false };

    // TV video config — UI writes, DemodThread reads
    std::atomic<TvStandard> tv_standard                   { TvStandard::NTSC };
    std::atomic<float>      tv_sync_threshold             { -0.5f };
    std::atomic<bool>       tv_audio_subcarrier_on        { true };
    std::atomic<double>     tv_audio_subcarrier_offset_hz { 6'500'000.0 };
    // Уровень белого. В настоящем композитном сигнале белое стоит на 100 IRE
    // при амплитуде синхроимпульса 40 IRE, отсюда 2.5 по умолчанию, а файлы
    // из mp4_to_iq.py кодируют белое как 0.85 того же размаха — поэтому
    // отношение вынесено в настройку, а не зашито константой. tv_white_auto
    // вместо этого тянет уровень белого за самой яркой частью картинки:
    // работает с любым источником, но на тёмной сцене задирает яркость.
    std::atomic<float>      tv_white_ratio                { 2.5f };
    std::atomic<bool>       tv_white_auto                 { false };

    // Шумоподавление — UI пишет, DemodThread читает.
    std::atomic<bool>       tv_repair_bad_lines           { true };
    // 0 = выключено; доля предыдущего кадра, подмешиваемая в текущий там, где
    // картинка неподвижна (по движущимся местам фильтр не работает, иначе
    // тянулся бы шлейф)
    std::atomic<float>      tv_temporal_nr                { 0.0f };

    // Диагностика ТВ-сигнала — DemodThread пишет, UI читает. Всё это декодер
    // и так измеряет по ходу дела; вынесено наружу, чтобы при кривой картинке
    // было видно, ЧТО именно не так — в первую очередь несовпадение частоты
    // дискретизации с записью (тогда измеренный период строки уезжает от
    // стандартных 63.5/64 usec, а картинка расплывается).
    std::atomic<float>      tv_diag_line_us               { 0.0f }; // измеренный период строки
    std::atomic<float>      tv_diag_field_hz              { 0.0f }; // измеренная частота полей
    std::atomic<float>      tv_diag_sync_level            { 0.0f }; // вершина синхроимпульса
    std::atomic<float>      tv_diag_black_level           { 0.0f }; // уровень гашения
    std::atomic<int>        tv_diag_lines_captured        { 0 };    // строк попало в кадр
    std::atomic<int>        tv_diag_lines_rejected        { 0 };    // импульсов отбраковано маховиком
    std::atomic<bool>       tv_diag_stream_gap            { false }; // поле короче номинала = разрыв потока

    // TV video demod output — DemodThread writes, display reads.
    // tv_frame is row-major RGB, tv_frame_width*tv_frame_height*3 bytes
    // (packed R,G,B per pixel). NTSC with no/low color-burst lock and all
    // PAL frames degenerate to R=G=B=Y automatically (no format branch).
    // width/height only ever change together with the buffer contents (NTSC/PAL
    // switch), so — like WATERFALL_ROWS's row width — they live under the same
    // mutex rather than as separate atomics.
    std::mutex              tv_frame_mutex;
    std::vector<uint8_t>    tv_frame;
    int                     tv_frame_width  { 320 };
    int                     tv_frame_height { 240 };
    std::atomic<bool>       tv_frame_has_data    { false };
    std::atomic<uint64_t>   tv_frame_update_count{ 0 };
    // true once the NTSC color-burst phase lock has enough confidence to
    // trust the current frame's chroma — only diagnostic a user has for
    // "why is this NTSC capture still gray" (drift, wrong sample rate, or
    // a porch too short for the burst window to fit)
    std::atomic<bool>       tv_color_locked { false };

    // TV video recording (MP4) — UI writes path/request, TvRecorderThread
    // owns the ffmpeg subprocess and all pipe I/O. Runs as its own thread so
    // an ffmpeg pipe write stalling on disk/encoder backpressure only ever
    // blocks that thread — never DemodThread's real-time DSP loop or
    // DisplayThread's render loop, both of which only ever touch the
    // tv_frame_mutex above, briefly, the same way they already did before
    // recording existed.
    std::mutex              tv_record_mutex;
    std::string             tv_record_file_path{ "data/tv_recording.mp4" };
    std::atomic<bool>       tv_record_requested     { false }; // one-shot: (re)start
    std::atomic<bool>       tv_record_stop_requested{ false }; // one-shot: stop
    std::atomic<bool>       tv_is_recording         { false };
    std::atomic<bool>       tv_record_error         { false }; // ffmpeg failed to launch, bad path, or pipe broke mid-recording
    std::atomic<uint64_t>   tv_record_frames_written{ 0 };

    // IQ playback config — UI writes, IqPlaybackThread reads. Lets a
    // recorded .bin (e.g. from python/mp4_to_iq.py, or a live "Save IQ"
    // capture) be replayed through the same fft_buf/df_buf/demod_buf
    // pipeline as live hardware, for testing without an SDR connected.
    std::mutex             playback_mutex;
    std::string            playback_file_path{ "" };
    std::atomic<bool>      playback_requested     { false }; // one-shot: (re)start playback of playback_file_path
    std::atomic<bool>      playback_stop_requested{ false }; // one-shot: stop if currently playing
    std::atomic<bool>      playback_loop          { false };
    // Запись, сделанная в режиме Both, содержит 4 int16 на отсчёт (i1,q1,i2,q2),
    // одноканальная — 2 (i,q). Ни по размеру, ни по содержимому они неразличимы,
    // а разбор двухканального файла как одноканального принимает отсчёты Rx2 за
    // следующие по времени отсчёты Rx1 — то есть портит поток, а не просто
    // теряет второй канал. Поэтому формат задаётся снаружи: UI ставит галочку
    // (подставляя её по суффиксу _2ch в имени), IqPlaybackThread читает флаг при
    // открытии файла и держит его до конца воспроизведения.
    std::atomic<bool>      playback_dual_channel  { false };
    std::atomic<bool>      is_playing_back        { false };

    // IQ scope output — IqPlaybackThread writes a rolling window of the raw
    // I/Q it's currently replaying, display reads. For visually inspecting
    // a recording (distinct from the demodulated-audio waveform panel).

    AppState()                           = default;
    AppState(const AppState&)            = delete;
    AppState& operator=(const AppState&) = delete;
};