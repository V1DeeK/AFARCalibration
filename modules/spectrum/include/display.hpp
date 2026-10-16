#pragma once

#include "app_state.hpp"
#include <vector>

// Forward declarations keep ImGui/ImPlot out of users of the renderer header.
struct GLFWwindow;
struct ImGuiContext;
struct ImPlotContext;

class SpectrumRenderer {
public:
    enum class Page { Spectrum, Peleng, Phase };
    enum class Language { Russian, English };

    explicit SpectrumRenderer(AppState& state);
    ~SpectrumRenderer();

    bool initialize();
    void shutdown();
    void renderFrame(float width, float height, float devicePixelRatio, float deltaSeconds);
    ImGuiContext* imguiContext() const noexcept { return main_imgui_ctx_; }
    void setPage(Page page) noexcept { active_page_ = page; }
    Page page() const noexcept { return active_page_; }
    void setLanguage(Language) noexcept {}  // интерфейс только на русском
    Language language() const noexcept { return language_; }
    void setLightTheme(bool light) noexcept { light_theme_ = light; theme_dirty_ = true; }
    bool lightTheme() const noexcept { return light_theme_; }
    void syncTextFieldsFromState();

private:
    void render_page_navigation();
    void render_spectrum_contents();
    void copy_state();        // lock psd_mutex, snapshot vectors, unlock
    void render_psd_plot();   // one or two (dual-channel) PSD panes side by side
    void render_waterfall();  // one or two (dual-channel) waterfall panes side by side
    void render_controls();   // sliders, dropdowns, buttons — Phase 7

    void copy_demod_state();  // lock demod_audio_mutex, snapshot waveform, unlock
    void render_demod_pane(); // FM-audio waveform plot — in-window, not detachable

    void copy_clip_state();   // lock clip_mutex, snapshot the clipping history, unlock

    void copy_phase_hist_state(); // lock phase_hist_mutex, snapshot the Rx1-Rx2 phase history, unlock

    // Legacy independent phase window support; the normal entry point is now
    // the phase-history page in the main navigation bar.
    void open_phase_window();
    void close_phase_window();
    void render_phase_frame();
    void draw_phase_contents();
    // Полоса уровня и перегрузки под водопадом. Отдельная панель, а не пометки
    // на спектре: клиппинг — это про время, а не про частоту, и в спектре он
    // выглядит как гребёнка гармоник, то есть как обычный сигнал.
    void render_clip_pane();

    // Панель управления разбита на секции по смыслу: раньше все контролы шли
    // одним потоком и было не понять, что к чему относится и что вообще нужно
    // трогать. Порядок секций — от «смотрю, работает ли» к «настраиваю».
    void render_status_bar();      // подключение, частота, темп, предупреждения
    void render_device_section();  // выбор устройства и параметры захвата
    void render_tuning_section();  // частота и усиление — то, что крутят постоянно
    void render_gain_controls();   // режим АРУ, уровень по каналам, обратная связь от платы
    void render_display_section(); // размер БПФ, калибровка, удержание пика, курсор
    void render_demod_section();   // режим демодуляции и его настройки, звук
    void render_record_section();  // запись IQ и воспроизведение файла
    // подпись слева от поля с выравниванием по колонке (ImGui ставит справа)
    void labeled_item(const char* label, float item_width);
    const char* text(const char* russian, const char* english) const noexcept;
    static constexpr float kLabelColumnWidth  = 96.0f;
    // сколько по высоте резервируется под панель управления (она прокручиваемая,
    // так что содержимое влезет в любом случае)
    // Высоты блоков заданы явно — так раскладка не «дышит» при каждом
    // изменении содержимого. Панель управления получает то, что осталось.
    static constexpr float kControlsPaneHeight = 220.0f; // минимум под настройки
    static constexpr float kPsdHeight          = 320.0f; // спектр
    static constexpr float kWaterfallHeight    = 200.0f; // водопад — это история сигнала
    static constexpr float kAudioPaneHeight    = 175.0f; // осциллограмма звука
    static constexpr float kClipPaneHeight     = 135.0f; // уровень АЦП и моменты перегрузки

    // single-pane renderers shared by the mono and dual-channel layouts
    // second_channel выбирает, чьи показания SNR накладывать на график: в
    // режиме Both их два, по одному на панель.
    void render_psd_pane(const char* title, const std::vector<float>& psd, const std::vector<float>& peak,
                          float plot_w, float plot_h, double freq_start, double freq_stop,
                          bool second_channel);
    void render_waterfall_pane(const char* title, const std::vector<float>& waterfall, float plot_w, float plot_h,
                                double freq_start, double freq_stop);

    void copy_peleng_state();     // lock peleng_mutex, snapshot subband result vectors, unlock

    // Peleng window is a genuinely independent OS window — its own GLFWwindow +
    // ImGui/ImPlot context — rather than an ImGui floating window confined to
    // the main GLFW surface. This ImGui build has no ImGuiConfigFlags_ViewportsEnable
    // to detach floating windows, so a second real window is the only way to get
    // one the user can drag/resize/close independently of the main window.
    void open_peleng_window();    // lazily create the second GLFWwindow + contexts
    void close_peleng_window();   // destroy them
    void render_peleng_frame();   // one full standalone frame: make current, draw, swap
    void draw_peleng_contents();  // the actual widgets/plots (assumes peleng context is current)
    void draw_peleng_source_controls(); // local-IQ vs board-over-TCP source picker
    void draw_peleng_calibration_status(); // whether C1[p]/C2[p] reached the pipeline
    void draw_peleng_spectrum_tab();    // the four per-frequency plots (Sigma, rho, SNR, gamma^2)
    void draw_peleng_rho_plane_tab();   // rho on the complex plane at one selected frequency
    void draw_peleng_phase_history_tab(); // Rx2/Rx1 phase of the selected subband over time
    void draw_peleng_table_tab();       // per-subband numbers as a sortable/filterable table
    void draw_peleng_log_controls();    // CSV log start/stop — shared by both tabs
    // ближайшая к peleng_sel_freq_mhz_ подполоса, или -1 если данных нет
    int  peleng_selected_subband() const;
    // дописывает точку в след, если пришёл новый блок статистик
    void update_peleng_trail(int subband);

    void copy_tv_state();       // lock tv_frame_mutex, snapshot frame buffer + dims, unlock

    // TV video window — third detachable GLFWwindow, same independent-window
    // pattern as Peleng (this ImGui build has no viewport support). First
    // use of a raw OpenGL texture + ImGui::Image() in this codebase.
    void open_tv_window();
    void close_tv_window();
    void render_tv_frame();
    void draw_tv_contents();
    void update_tv_texture();  // (re)upload local_tv_frame_ into tv_texture_
    void draw_tv_diagnostics(); // измеренные параметры сигнала — видно, что именно не так


    AppState& state_;
    Page active_page_{ Page::Spectrum };
    Language language_{ Language::Russian };
    bool light_theme_{ false };
    bool theme_dirty_{ true };
    bool show_controls_{ true };

    // local copies so psd_mutex isn't held during rendering
    // ch2 copies are only meaningful when chan_mode == Both
    std::vector<float> local_psd_;
    std::vector<float> local_peak_;
    std::vector<float> local_waterfall_;
    std::vector<float> local_psd2_;
    std::vector<float> local_peak2_;
    std::vector<float> local_waterfall2_;
    int                local_fft_size_{ 0 };

    // demod (FM audio) local copy — DemodThread writes state_.demod_audio_*, we
    // snapshot under demod_audio_mutex each frame
    std::vector<float> local_demod_waveform_;

    // История перегрузки — снимок под clip_mutex, как и всё остальное, что
    // пишут рабочие потоки. clip_x_ и clip_mark_* пересобираются каждый кадр
    // из этого снимка: ось времени рисуется относительно «сейчас».
    std::vector<float> local_clip_t_;
    std::vector<float> local_clip_pct1_;
    std::vector<float> local_clip_pct2_;
    std::vector<float> local_clip_dbfs1_;
    std::vector<float> local_clip_dbfs2_;
    std::vector<float> clip_x_;
    std::vector<float> clip_mark_x_;
    std::vector<float> clip_mark_y_;

    // История Rx1-Rx2 фазы — снимок под phase_hist_mutex, ось строится каждый
    // кадр как и clip_x_ выше: время «сейчас минус t» в секундах.
    std::vector<float> local_phase_hist_time_s_;
    std::vector<float> local_phase_hist_deg_;
    std::vector<float> phase_hist_x_;

    bool                  show_phase_window_{ false };
    GLFWwindow*           phase_window_{ nullptr };
    ImGuiContext*         phase_imgui_ctx_{ nullptr };
    ImPlotContext*        phase_implot_ctx_{ nullptr };

    // IQ playback: file path input buffer (same convention as ip_input_buf_)
    // and the local copy of the raw I/Q scope, written by IqPlaybackThread
    char                playback_path_buf_[256]{ "data/recording.bin" };
    // последний результат разбора имени на суффикс _2ch: галочка числа каналов
    // подставляется только когда признак в имени МЕНЯЕТСЯ, иначе каждое нажатие
    // клавиши затирало бы выбор, сделанный вручную
    bool                playback_path_was_2ch_{ false };

    // имя файла записи IQ; пустое — автоматическое имя (см. save_iq_file)
    char                save_name_buf_[256]{ "" };

    ImGuiContext*  main_imgui_ctx_{ nullptr };
    ImPlotContext* main_implot_ctx_{ nullptr };

    // cursor state for dBm readout
    bool  cursor_locked_{ false };
    float cursor_freq_hz_{ 0.0f };
    float cursor_dbm_{ 0.0f };

    // manual IP/URI entry buffer for hardware connection
    char rx_cal_path_buf_[256]{ "data/rx_calibration.csv" };
    char ip_input_buf_[128]{ "ip:172.16.226." };
    // MAC платы: постоянный, в отличие от её DHCP-адреса — по нему и ищем
    char board_mac_buf_[32]{ "42:7e:d7:4e:0f:77" };

    // x-axis frequency array recomputed when fft_size, center_freq, or sample_rate changes
    std::vector<float> freq_axis_;
    uint64_t           last_center_freq_{ 0 };  // detect freq changes for axis rebuild
    uint32_t           last_sample_rate_hz_{ 0 }; // detect sample rate changes for axis rebuild

    // peleng (direction finding) window — opened via a button on the main window,
    // rendered as an independent GLFW window with its own ImGui/ImPlot context
    bool                  show_peleng_window_{ false };
    GLFWwindow*           peleng_window_{ nullptr };
    ImGuiContext*         peleng_imgui_ctx_{ nullptr };
    ImPlotContext*        peleng_implot_ctx_{ nullptr };
    std::vector<float>    peleng_freq_mhz_;
    std::vector<float>    peleng_rho_re_;
    std::vector<float>    peleng_rho_im_;
    std::vector<float>    peleng_gamma2_;
    std::vector<float>    peleng_snr_db_;
    std::vector<float>    peleng_sigma_dbfs_;
    std::vector<uint8_t>  peleng_valid_;
    uint64_t              peleng_update_count_{ 0 };

    // Table tab: row order (indices into the arrays above), rebuilt each frame
    // from the current filter/sort so the other tabs' arrays stay untouched —
    // they rely on frequency order.
    std::vector<int>      peleng_table_rows_;
    bool                  peleng_table_valid_only_{ false };

    // След rho на комплексной плоскости для одной выбранной подполосы: каждый
    // новый блок статистик добавляет точку (Re, Im), последняя рисуется красной.
    // Ведётся здесь, а не в потоке пеленгации, потому что это чисто вопрос
    // отображения; точки берутся из тех же снимков, что и остальные графики,
    // так что при частоте блоков выше частоты кадров в след попадёт только
    // последний блок каждого кадра (полная история — в CSV-журнале).
    double                peleng_sel_freq_mhz_{ 0.0 };
    bool                  peleng_sel_valid_{ false };   // false, пока частота не выбрана/не инициализирована
    std::vector<float>    peleng_trail_re_;
    std::vector<float>    peleng_trail_im_;
    // время (glfwGetTime(), секунды) каждой точки следа — параллельно re_/im_
    // выше, нужно только для оси времени вкладки Phase history.
    std::vector<float>    peleng_trail_time_s_;
    int                   peleng_trail_len_{ 256 };     // сколько точек держать, настраивается в UI
    int                   peleng_trail_subband_{ -1 };  // подполоса, которой принадлежит текущий след
    uint64_t              peleng_trail_last_update_{ 0 };
    static constexpr int  kPelengTrailMax = 4096;

    // Rx2/Rx1 фаза следа, пересчитанная из peleng_trail_re_/im_ — рабочие
    // буферы вкладки Phase history, пересобираются каждый кадр из следа.
    std::vector<float>    peleng_phase_hist_deg_;
    std::vector<float>    peleng_phase_hist_x_;

    // путь к CSV-журналу пеленга, та же схема, что у playback_path_buf_
    char                  peleng_log_path_buf_[256]{ "data/peleng_log.csv" };

    // TV video (monochrome demod) window — opened via a button on the main
    // window, rendered as an independent GLFW window with its own ImGui/
    // ImPlot context, following the exact Peleng pattern above
    bool                  show_tv_window_{ false };
    GLFWwindow*           tv_window_{ nullptr };
    ImGuiContext*         tv_imgui_ctx_{ nullptr };
    ImPlotContext*        tv_implot_ctx_{ nullptr };
    std::vector<uint8_t>  local_tv_frame_;
    int                   local_tv_frame_width_{ 0 };
    int                   local_tv_frame_height_{ 0 };
    uint64_t              tv_frame_update_count_{ 0 };

    // raw OpenGL texture backing the ImGui::Image() draw — GLuint is just
    // unsigned int; using that here avoids pulling GL headers into this
    // header (same forward-declare discipline as GLFWwindow/ImGuiContext above)
    unsigned int          tv_texture_{ 0 };
    int                   tv_texture_width_{ 0 };
    int                   tv_texture_height_{ 0 };

    // TV recording: output path input buffer, same convention as
    // playback_path_buf_ — TvRecorderThread owns the actual ffmpeg process
    char                  record_path_buf_[256]{ "data/tv_recording.mp4" };
};
