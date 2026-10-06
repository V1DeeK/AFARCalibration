#include "display.hpp"

#include "clip_monitor.hpp"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"
#include "implot.h"

#include <iostream>
#include <algorithm>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <limits>

using namespace std;

// static constexpr float SAMPLE_RATE = 40.0e6f;
static constexpr float DBM_MIN     = -120.0f;
static constexpr float DBM_MAX     =   10.0f;

// GL 3.x texture constants not present in imgui_impl_opengl3_loader.h (which
// only defines what ImGui's own renderer backend needs internally) — values
// are the standard, stable OpenGL enum assignments.
#ifndef GL_RGB
#define GL_RGB 0x1907
#endif
#ifndef GL_RGB8
#define GL_RGB8 0x8051
#endif
// GL 1.2 constant absent from the compact loader used by ImGui.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

SpectrumRenderer::SpectrumRenderer(AppState& state)
    : state_(state) {}

SpectrumRenderer::~SpectrumRenderer() {
    shutdown();
}

void SpectrumRenderer::syncTextFieldsFromState() {
    {
        lock_guard<mutex> lock(state_.connection_mutex);
        snprintf(ip_input_buf_, sizeof(ip_input_buf_), "%s", state_.pluto_ip.c_str());
    }
    {
        lock_guard<mutex> lock(state_.rx_cal_mutex);
        snprintf(rx_cal_path_buf_, sizeof(rx_cal_path_buf_), "%s", state_.rx_cal_path.c_str());
    }
    {
        lock_guard<mutex> lock(state_.playback_mutex);
        snprintf(playback_path_buf_, sizeof(playback_path_buf_), "%s",
                 state_.playback_file_path.c_str());
    }
    {
        lock_guard<mutex> lock(state_.peleng_log_mutex);
        snprintf(peleng_log_path_buf_, sizeof(peleng_log_path_buf_), "%s",
                 state_.peleng_log_path.c_str());
    }
    {
        lock_guard<mutex> lock(state_.tv_record_mutex);
        snprintf(record_path_buf_, sizeof(record_path_buf_), "%s",
                 state_.tv_record_file_path.c_str());
    }
}

bool SpectrumRenderer::initialize() {
    if (main_imgui_ctx_)
        return true;
    IMGUI_CHECKVERSION();
    main_imgui_ctx_  = ImGui::CreateContext();
    main_implot_ctx_ = ImPlot::CreateContext();
    ImGui::SetCurrentContext(main_imgui_ctx_);
    ImPlot::SetCurrentContext(main_implot_ctx_);

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui::GetStyle().FrameRounding  = 3.0f;

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "spectrum_qt";
    return ImGui_ImplOpenGL3_Init("#version 330");
}

void SpectrumRenderer::shutdown() {
    if (!main_imgui_ctx_)
        return;
    ImGui::SetCurrentContext(main_imgui_ctx_);
    ImPlot::SetCurrentContext(main_implot_ctx_);

    if (tv_texture_) {
        glDeleteTextures(1, &tv_texture_);
        tv_texture_ = 0;
        tv_texture_width_ = 0;
        tv_texture_height_ = 0;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext(main_implot_ctx_);
    ImGui::DestroyContext(main_imgui_ctx_);
    main_implot_ctx_ = nullptr;
    main_imgui_ctx_  = nullptr;
}

void SpectrumRenderer::renderFrame(float width, float height, float devicePixelRatio,
                                   float deltaSeconds) {
    if (!main_imgui_ctx_ || width <= 0.0f || height <= 0.0f)
        return;

    copy_state();
    copy_peleng_state();
    copy_demod_state();
    copy_tv_state();
    copy_clip_state();
    copy_phase_hist_state();

    ImGui::SetCurrentContext(main_imgui_ctx_);
    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(devicePixelRatio, devicePixelRatio);
    io.DeltaTime = std::max(deltaSeconds, 1.0f / 1000.0f);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    render_page_navigation();
    ImGui::Separator();

    switch (active_page_) {
    case Page::Spectrum:
        render_spectrum_contents();
        break;
    case Page::Peleng:
        ImGui::BeginChild("##peleng_page");
        draw_peleng_contents();
        ImGui::EndChild();
        break;
    case Page::Television:
        update_tv_texture();
        ImGui::BeginChild("##tv_page");
        draw_tv_contents();
        ImGui::EndChild();
        break;
    case Page::Phase:
        ImGui::BeginChild("##phase_page");
        draw_phase_contents();
        ImGui::EndChild();
        break;
    }
    ImGui::End();

    ImGui::Render();
    const int framebufferWidth = static_cast<int>(width * devicePixelRatio);
    const int framebufferHeight = static_cast<int>(height * devicePixelRatio);
    glViewport(0, 0, framebufferWidth, framebufferHeight);
    glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void SpectrumRenderer::render_page_navigation() {
    static constexpr const char* labels[] = {
        "Spectrum", "Peleng", "TV", "Rx1/Rx2 phase"
    };
    if (!ImGui::BeginTable("##spectrum_pages", 4, ImGuiTableFlags_SizingStretchSame))
        return;

    for (int index = 0; index < 4; ++index) {
        ImGui::TableNextColumn();
        const auto page = static_cast<Page>(index);
        if (ImGui::Selectable(labels[index], active_page_ == page, 0, ImVec2(-FLT_MIN, 0.0f)))
            active_page_ = page;
    }
    ImGui::EndTable();
}

void SpectrumRenderer::render_spectrum_contents() {
    const bool audioPane = state_.demod_mode.load() != DemodMode::Off
                        && state_.demod_audio_has_data.load();
    const bool clipPane = state_.clip_stream_active.load() || !local_clip_t_.empty();
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    float plotsHeight = kPsdHeight + kWaterfallHeight
                      + (audioPane ? kAudioPaneHeight : 0.0f)
                      + (clipPane ? kClipPaneHeight : 0.0f) + spacing * 5.0f;
    plotsHeight = std::max(1.0f,
        std::min(plotsHeight, ImGui::GetContentRegionAvail().y - kControlsPaneHeight));
    ImGui::BeginChild("##plots", ImVec2(0.0f, plotsHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    if (ImPlot::BeginAlignedPlots("psd_waterfall_align")) {
        render_psd_plot();
        ImGui::Separator();
        render_waterfall();
        ImPlot::EndAlignedPlots();
    }
    if (clipPane) {
        ImGui::Separator();
        render_clip_pane();
    }
    ImGui::Separator();
    render_demod_pane();
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginChild("##controls_scroll", ImVec2(0.0f, 0.0f));
    render_controls();
    ImGui::EndChild();
}

void SpectrumRenderer::copy_state() {
    lock_guard<mutex> lock(state_.psd_mutex);
    local_fft_size_   = static_cast<int>(state_.psd_dbm.size());
    local_psd_        = state_.psd_dbm;
    local_peak_       = state_.peak_dbm;
    local_waterfall_  = state_.waterfall;
    local_psd2_       = state_.psd_dbm2;
    local_peak2_      = state_.peak_dbm2;
    local_waterfall2_ = state_.waterfall2;
}

void SpectrumRenderer::copy_demod_state() {
    lock_guard<mutex> lock(state_.demod_audio_mutex);
    local_demod_waveform_ = state_.demod_audio_waveform;
}

void SpectrumRenderer::copy_clip_state() {
    lock_guard<mutex> lock(state_.clip_mutex);
    local_clip_t_     = state_.clip_hist_time_s;
    local_clip_pct1_  = state_.clip_hist_pct1;
    local_clip_pct2_  = state_.clip_hist_pct2;
    local_clip_dbfs1_ = state_.clip_hist_dbfs1;
    local_clip_dbfs2_ = state_.clip_hist_dbfs2;
}

void SpectrumRenderer::copy_phase_hist_state() {
    lock_guard<mutex> lock(state_.phase_hist_mutex);
    local_phase_hist_time_s_ = state_.phase_hist_time_s;
    local_phase_hist_deg_    = state_.phase_hist_deg;
}

void SpectrumRenderer::copy_tv_state() {
    lock_guard<mutex> lock(state_.tv_frame_mutex);
    local_tv_frame_        = state_.tv_frame;
    local_tv_frame_width_  = state_.tv_frame_width;
    local_tv_frame_height_ = state_.tv_frame_height;
    tv_frame_update_count_ = state_.tv_frame_update_count.load();
}

void SpectrumRenderer::render_psd_pane(const char* title, const std::vector<float>& psd, const std::vector<float>& peak,
                                     float plot_w, float plot_h, double freq_start, double freq_stop,
                                     bool second_channel) {
    int    n    = static_cast<int>(psd.size());
    double span = freq_stop - freq_start;

    // Показания SNR по этому каналу: шумовой пол считает ProcessingThread по
    // полному спектру (см. publish_snr), здесь только разметка.
    bool  snr_valid = second_channel ? state_.snr_valid2.load()     : state_.snr_valid.load();
    float snr_db    = second_channel ? state_.snr_db2.load()        : state_.snr_db.load();
    float noise_dbm = second_channel ? state_.snr_noise_dbm2.load() : state_.snr_noise_dbm.load();
    float peak_dbm  = second_channel ? state_.snr_peak_dbm2.load()  : state_.snr_peak_dbm.load();
    double peak_hz  = second_channel ? state_.snr_peak_hz2.load()   : state_.snr_peak_hz.load();
    bool  overlay   = snr_valid && state_.snr_overlay_on.load();

    ImPlot::SetNextAxesLimits(freq_start, freq_stop, DBM_MIN, DBM_MAX, ImPlotCond_Always);

    if (ImPlot::BeginPlot(title, { plot_w, plot_h }, ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes("Frequency (MHz)", "Power (dBm)");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_start, freq_stop, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, DBM_MIN, DBM_MAX, ImPlotCond_Always);

        if (n > 0) {
            ImPlotSpec psd_spec;
            psd_spec.LineColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
            ImPlot::PlotLine(title, freq_axis_.data(), psd.data(), n, psd_spec);

            if (state_.peak_hold_on.load() && !peak.empty()) {
                ImPlotSpec peak_spec;
                peak_spec.LineColor = ImVec4(1.0f, 0.4f, 0.1f, 1.0f);
                ImPlot::PlotLine("Peak", freq_axis_.data(), peak.data(), n, peak_spec);
            }

            // Шумовой пол линией и SNR подписью у пика: без этой линии «SNR 30 дБ»
            // — просто число, а с ней видно, от чего оно отсчитано и там ли
            // вообще стоит пол, где шум.
            if (overlay) {
                ImPlotSpec nf_spec;
                nf_spec.LineColor = ImVec4(0.65f, 0.65f, 0.70f, 0.75f);
                float nf_x[2] = { static_cast<float>(freq_start), static_cast<float>(freq_stop) };
                float nf_y[2] = { noise_dbm, noise_dbm };
                ImPlot::PlotLine("Noise floor", nf_x, nf_y, 2, nf_spec);

                float peak_mhz = static_cast<float>(peak_hz / 1e6);
                ImPlotSpec mark_spec;
                mark_spec.LineColor       = ImVec4(1.0f, 0.85f, 0.2f, 1.0f);
                mark_spec.MarkerFillColor = ImVec4(1.0f, 0.85f, 0.2f, 0.0f);
                mark_spec.Marker          = ImPlotMarker_Circle;
                mark_spec.MarkerSize      = 5.0f;
                ImPlot::PlotScatter("##snr_peak", &peak_mhz, &peak_dbm, 1, mark_spec);

                ImPlot::Annotation(peak_mhz, peak_dbm, ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                    ImVec2(0.0f, -10.0f), true,
                                    "SNR %.1f dB  (%.1f dBm)", snr_db, peak_dbm);
                ImPlot::Annotation(freq_start + span * 0.02, noise_dbm,
                                    ImVec4(0.75f, 0.75f, 0.80f, 1.0f), ImVec2(0.0f, -9.0f), true,
                                    "noise %.1f dBm", noise_dbm);
            }
        }

        if (n > 0 && ImPlot::IsPlotHovered()) {
            ImPlotPoint mouse = ImPlot::GetPlotMousePos();

            if (!cursor_locked_) {
                cursor_freq_hz_ = static_cast<float>(mouse.x * 1e6);
                int bin = static_cast<int>((mouse.x - freq_start) / span * n);
                bin = max(0, min(n - 1, bin));
                cursor_dbm_ = psd[bin];
            }

            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                cursor_locked_ = !cursor_locked_;

            ImGui::BeginTooltip();
            ImGui::Text("%.4f MHz", cursor_freq_hz_ / 1e6f);
            ImGui::Text("%.1f dBm", cursor_dbm_);
            // «Над шумом» — тот же шумовой пол, что нарисован линией; для узкого
            // сигнала это и есть его SNR, а не только SNR самого сильного в полосе.
            if (snr_valid) ImGui::Text("%+.1f dB over noise", cursor_dbm_ - noise_dbm);
            if (cursor_locked_) ImGui::Text("[locked]");
            ImGui::EndTooltip();

            ImPlotSpec cur_spec;
            cur_spec.LineColor = ImVec4(1.0f, 1.0f, 0.0f, 0.8f);
            float cx    = cursor_freq_hz_ / 1e6f;
            float ys[2] = { DBM_MIN, DBM_MAX };
            float xs[2] = { cx, cx };
            ImPlot::PlotLine("##cursor", xs, ys, 2, cur_spec);
        }

        ImPlot::EndPlot();
    }
}

void SpectrumRenderer::render_psd_plot() {
    int    n          = local_fft_size_;
    double center     = static_cast<double>(state_.center_freq_hz.load()) / 1e6;
    double span       = static_cast<double>(state_.sample_rate_hz.load()) / 1e6;
    double freq_start = center - span / 2.0;
    double freq_stop  = center + span / 2.0;

    // rebuild freq axis whenever center freq, fft size, or sample rate changes
    uint64_t current_center = state_.center_freq_hz.load();
    uint32_t current_sample_rate = state_.sample_rate_hz.load();
    if (static_cast<int>(freq_axis_.size()) != n || current_center != last_center_freq_ ||
        current_sample_rate != last_sample_rate_hz_) {
        last_center_freq_ = current_center;
        last_sample_rate_hz_ = current_sample_rate;
        freq_axis_.resize(n);
        for (int k = 0; k < n; ++k)
            freq_axis_[k] = static_cast<float>(freq_start + k * (span / n));
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Высота задана константой, а не долей от окна: растянутый на пол-экрана
    // спектр не показывает ничего сверх того, что видно в умеренной высоте.
    float reserved = kWaterfallHeight + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    if (state_.demod_mode.load() != DemodMode::Off && state_.demod_audio_has_data.load())
        reserved += kAudioPaneHeight;
    if (state_.clip_stream_active.load() || !local_clip_t_.empty())
        reserved += kClipPaneHeight;
    float h = std::clamp(avail.y - reserved, 180.0f, kPsdHeight);

    // dual-channel capture (Rx1 + Rx2) gets two independent PSD panes side by side
    bool dual = (state_.chan_mode.load() == PlutoChannelMode::Both) && !local_psd2_.empty();
    if (dual) {
        float pane_w = avail.x * 0.5f - 4.0f;
        render_psd_pane("PSD Rx1", local_psd_, local_peak_, pane_w, h, freq_start, freq_stop, false);
        ImGui::SameLine();
        render_psd_pane("PSD Rx2", local_psd2_, local_peak2_, pane_w, h, freq_start, freq_stop, true);
    } else {
        render_psd_pane("PSD", local_psd_, local_peak_, avail.x, h, freq_start, freq_stop, false);
    }
}

void SpectrumRenderer::render_waterfall_pane(const char* title, const std::vector<float>& waterfall,
                                           float plot_w, float plot_h, double freq_start, double freq_stop) {
    // force X axis to track center freq, hide Y axis ticks (time is implicit)
    ImPlot::SetNextAxesLimits(freq_start, freq_stop, 0, AppState::WATERFALL_ROWS, ImPlotCond_Always);

    if (ImPlot::BeginPlot(title, { plot_w, plot_h },
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes("Frequency (MHz)", nullptr,
                          ImPlotAxisFlags_None,
                          ImPlotAxisFlags_NoDecorations);
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_start, freq_stop, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0, AppState::WATERFALL_ROWS, ImPlotCond_Always);

        if (!waterfall.empty() && local_fft_size_ > 0) {
            ImPlot::PushColormap(ImPlotColormap_Jet);
            ImPlot::PlotHeatmap("##wf",
                                waterfall.data(),
                                AppState::WATERFALL_ROWS,
                                local_fft_size_,
                                DBM_MIN, DBM_MAX,
                                nullptr,
                                ImPlotPoint(freq_start, 0),
                                ImPlotPoint(freq_stop, static_cast<double>(AppState::WATERFALL_ROWS)));
            ImPlot::PopColormap();
        }

        ImPlot::EndPlot();
    }
}

void SpectrumRenderer::render_waterfall() {
    double center     = static_cast<double>(state_.center_freq_hz.load()) / 1e6;
    double span       = static_cast<double>(state_.sample_rate_hz.load()) / 1e6;
    double freq_start = center - span / 2.0;
    double freq_stop  = center + span / 2.0;

    ImVec2 avail = ImGui::GetContentRegionAvail();

    // Высота водопада — остаток по вертикали за вычетом панели звука, которая
    // идёт ниже и показывается только при включённой демодуляции.
    // Фиксированные 400 px переполняли окно, и нижние панели не помещались.
    float reserved = 0.0f;
    if (state_.demod_mode.load() != DemodMode::Off && state_.demod_audio_has_data.load())
        reserved += kAudioPaneHeight;
    if (state_.clip_stream_active.load() || !local_clip_t_.empty())
        reserved += kClipPaneHeight;
    float h = std::clamp(avail.y - reserved, 120.0f, kWaterfallHeight);

    bool dual = (state_.chan_mode.load() == PlutoChannelMode::Both) && !local_waterfall2_.empty();
    if (dual) {
        float pane_w = avail.x * 0.5f - 4.0f;
        render_waterfall_pane("Waterfall Rx1", local_waterfall_, pane_w, h, freq_start, freq_stop);
        ImGui::SameLine();
        render_waterfall_pane("Waterfall Rx2", local_waterfall2_, pane_w, h, freq_start, freq_stop);
    } else {
        render_waterfall_pane("Waterfall", local_waterfall_, avail.x, h, freq_start, freq_stop);
    }
}

// Полоса уровня АЦП и моментов перегрузки под водопадом.
//
// Отвечает на вопрос «когда именно зажимало»: спектр этого не показывает —
// перегрузка размазывается по всей полосе гребёнкой гармоник и выглядит как
// богатый сигнал, а водопад её показывает как общее посветление, которое так
// же выглядит и при честном сильном сигнале.
void SpectrumRenderer::render_clip_pane() {
    PlutoChannelMode mode = state_.chan_mode.load();
    // Наличие второго канала берём из самих данных, а не из режима приёмника:
    // двухканальный файл воспроизводится и при chan_mode == Channel_1, и второй
    // тракт в нём такой же настоящий, как на живом потоке.
    bool dual = state_.clip_peak_dbfs2.load() > -120.0f;
    const char* name1 = (mode == PlutoChannelMode::Channel_2) ? "Rx2" : "Rx1";

    // Порог зажатия в единицах графика — та же величина, что ClipMonitor::kClipCode.
    const float clip_dbfs = 20.0f * std::log10(static_cast<float>(ClipMonitor::kClipCode) / ClipMonitor::kFullScale);

    uint64_t windows   = state_.clip_windows.load();
    uint64_t win_total = state_.clip_windows_total.load();
    float    worst     = state_.clip_worst_pct.load();
    double   t_now     = state_.clip_stream_time_s.load();
    double   t_last    = state_.clip_last_time_s.load();

    if (windows > 0) {
        // Доля окон — это и есть то самое «часто»: одно окно за десять минут и
        // половина всех окон подряд требуют разных решений.
        float share = win_total ? 100.0f * static_cast<float>(windows) / static_cast<float>(win_total) : 0.0f;
        ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f},
                            "CLIPPING: %llu of %llu windows (%.0f%% of the time), worst %.2f%% of samples, last %.1f s ago",
                            static_cast<unsigned long long>(windows),
                            static_cast<unsigned long long>(win_total),
                            share, worst, t_last >= 0.0 ? t_now - t_last : 0.0);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Samples that hit the ADC full scale. They are destroyed, not just loud:\n"
                              "the spectrum grows harmonics that were never on the air, and demodulation\n"
                              "breaks on them. Cure is less gain (or an attenuator), not a different view.");
    } else if (dual) {
        ImGui::TextDisabled("ADC level: %s peak %.1f dBFS, Rx2 peak %.1f dBFS - no clipping",
                            name1, state_.clip_peak_dbfs1.load(), state_.clip_peak_dbfs2.load());
    } else {
        ImGui::TextDisabled("ADC level: %s peak %.1f dBFS - no clipping",
                            name1, state_.clip_peak_dbfs1.load());
    }

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset clip stats")) {
        state_.clip_reset_requested.store(true);
        // Пока поток не идёт, забирать флаг некому — чистим сами, иначе кнопка
        // выглядела бы сломанной именно тогда, когда на неё чаще всего жмут:
        // после просмотра записи.
        if (!state_.clip_stream_active.load()) {
            state_.clip_windows.store(0);
            state_.clip_windows_total.store(0);
            state_.clip_worst_pct.store(0.0f);
            state_.clip_last_time_s.store(-1.0);
            state_.clip_active.store(false);
            lock_guard<mutex> lock(state_.clip_mutex);
            state_.clip_hist_time_s.clear();
            state_.clip_hist_pct1.clear();
            state_.clip_hist_pct2.clear();
            state_.clip_hist_dbfs1.clear();
            state_.clip_hist_dbfs2.clear();
        }
    }

    int n = static_cast<int>(local_clip_t_.size());
    if (n == 0) {
        ImGui::TextDisabled("waiting for samples...");
        return;
    }

    // Ось времени — «сколько секунд назад», отсчитывается от последнего окна:
    // абсолютное время от начала потока читателю ничего не говорит, а «две
    // секунды назад» соотносится с тем, что он только что видел на водопаде.
    float t_end = local_clip_t_[n - 1];
    clip_x_.resize(n);
    for (int i = 0; i < n; ++i)
        clip_x_[i] = local_clip_t_[i] - t_end;

    clip_mark_x_.clear();
    clip_mark_y_.clear();
    for (int i = 0; i < n; ++i) {
        float pct = std::max(local_clip_pct1_[i], dual ? local_clip_pct2_[i] : 0.0f);
        if (pct > 0.0f) {
            clip_mark_x_.push_back(clip_x_[i]);
            clip_mark_y_.push_back(0.0f);
        }
    }

    double x_min = std::min(static_cast<double>(clip_x_[0]), -5.0);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  h     = std::clamp(avail.y, 60.0f, kClipPaneHeight - 30.0f);

    ImPlot::SetNextAxesLimits(x_min, 0.0, -60.0, 3.0, ImPlotCond_Always);
    if (ImPlot::BeginPlot("##clip", { avail.x, h }, ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes("Time (s, 0 = now)", "dBFS");
        ImPlot::SetupAxisLimits(ImAxis_X1, x_min, 0.0, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -60.0, 3.0, ImPlotCond_Always);
        // Полоса низкая (~90 px): легенда в столбик заняла бы её целиком, а
        // вынесенная наружу — отъедает ширину у самой истории.
        ImPlot::SetupLegend(ImPlotLocation_SouthEast, ImPlotLegendFlags_Horizontal);

        // Вертикальные красные линии — сами моменты перегрузки. Рисуются первыми,
        // чтобы кривая уровня шла поверх них, а не пряталась под ними.
        if (!clip_mark_x_.empty()) {
            ImPlotSpec mark_spec;
            mark_spec.LineColor  = ImVec4(1.0f, 0.25f, 0.25f, 0.55f);
            mark_spec.LineWeight = 1.0f;
            ImPlot::PlotInfLines("clipped", clip_mark_x_.data(), static_cast<int>(clip_mark_x_.size()), mark_spec);

            ImPlotSpec dot_spec;
            dot_spec.Marker          = ImPlotMarker_Square;
            dot_spec.MarkerSize      = 3.0f;
            dot_spec.LineColor       = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
            dot_spec.MarkerFillColor = ImVec4(1.0f, 0.25f, 0.25f, 1.0f);
            ImPlot::PlotScatter("##clip_dots", clip_mark_x_.data(), clip_mark_y_.data(),
                                 static_cast<int>(clip_mark_x_.size()), dot_spec);
        }

        // Порог зажатия. Всё, что его касается, и есть перегрузка — линия
        // избавляет от угадывания, далеко ли ещё до неё.
        ImPlotSpec thr_spec;
        thr_spec.LineColor = ImVec4(1.0f, 0.35f, 0.35f, 0.7f);
        float thr_x[2] = { static_cast<float>(x_min), 0.0f };
        float thr_y[2] = { clip_dbfs, clip_dbfs };
        ImPlot::PlotLine("full scale", thr_x, thr_y, 2, thr_spec);

        ImPlotSpec ch1_spec;
        ch1_spec.LineColor = ImVec4(0.2f, 0.8f, 0.2f, 1.0f);
        ImPlot::PlotLine(name1, clip_x_.data(), local_clip_dbfs1_.data(), n, ch1_spec);

        if (dual) {
            ImPlotSpec ch2_spec;
            ch2_spec.LineColor = ImVec4(0.224f, 0.529f, 0.898f, 1.0f);
            ImPlot::PlotLine("Rx2", clip_x_.data(), local_clip_dbfs2_.data(), n, ch2_spec);
        }

        // Подсказка по наведению: у окна есть точная доля зажатых отсчётов, а по
        // самой картинке видно только «было/не было».
        if (ImPlot::IsPlotHovered()) {
            double mx   = ImPlot::GetPlotMousePos().x;
            int    best = 0;
            for (int i = 1; i < n; ++i)
                if (std::fabs(clip_x_[i] - mx) < std::fabs(clip_x_[best] - mx)) best = i;

            ImGui::BeginTooltip();
            ImGui::Text("%.1f s ago", -clip_x_[best]);
            ImGui::Text("%s peak %.1f dBFS, clipped %.2f%%", name1, local_clip_dbfs1_[best], local_clip_pct1_[best]);
            if (dual)
                ImGui::Text("Rx2 peak %.1f dBFS, clipped %.2f%%", local_clip_dbfs2_[best], local_clip_pct2_[best]);
            ImGui::EndTooltip();
        }

        ImPlot::EndPlot();
    }
}

void SpectrumRenderer::render_demod_pane() {
    DemodMode mode = state_.demod_mode.load();
    if (mode == DemodMode::Off) return;

    ImGui::TextUnformatted("FM Audio Demod");

    if (!state_.demod_audio_has_data.load() || local_demod_waveform_.empty()) {
        ImGui::TextDisabled("Waiting for demodulated audio...");
        return;
    }

    int n = static_cast<int>(local_demod_waveform_.size());
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  h     = 140.0f;

    // x axis is just sample index within the rolling window — the panel is a
    // scope view, not a frequency-domain plot, so no axis units to track
    static std::vector<float> x_axis; // reused scratch, only rebuilt on size change
    if (static_cast<int>(x_axis.size()) != n) {
        x_axis.resize(n);
        for (int i = 0; i < n; ++i) x_axis[i] = static_cast<float>(i);
    }

    ImPlot::SetNextAxesLimits(0, n, -1.0, 1.0, ImPlotCond_Always);
    if (ImPlot::BeginPlot("Demodulated Audio", { avail.x, h },
                          ImPlotFlags_NoLegend | ImPlotFlags_NoInputs)) {
        ImPlot::SetupAxes("Sample", "Amplitude");
        ImPlot::SetupAxisLimits(ImAxis_X1, 0, n, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.0, 1.0, ImPlotCond_Always);

        ImPlotSpec spec;
        spec.LineColor = ImVec4(0.224f, 0.529f, 0.898f, 1.0f);
        ImPlot::PlotLine("audio", x_axis.data(), local_demod_waveform_.data(), n, spec);

        ImPlot::EndPlot();
    }
}

// Подпись слева от поля, с выравниванием по общей колонке. ImGui по
// умолчанию ставит подпись справа, из-за чего при плотной горизонтальной
// раскладке она сливается со следующим контролом и глаз не находит, к чему
// эта подпись относится.
void SpectrumRenderer::labeled_item(const char* label, float item_width) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(kLabelColumnWidth);
    ImGui::SetNextItemWidth(item_width);
}

// Строка состояния. Стоит первой и набрана цветом, а не серым мелким текстом
// внизу: это первое, на что нужно смотреть, когда что-то не работает.
void SpectrumRenderer::render_status_bar() {
    bool connected = state_.is_connected.load();
    bool playing   = state_.is_playing_back.load();

    ImGui::PushStyleColor(ImGuiCol_Text, connected ? ImVec4(0.35f, 0.9f, 0.35f, 1.0f)
                                                   : ImVec4(0.9f, 0.4f, 0.4f, 1.0f));
    ImGui::TextUnformatted(connected ? "* CONNECTED" : "* DISCONNECTED");
    ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::Text("|  %.3f MHz   %.1f MSPS   FFT %d   Frames %d",
                static_cast<double>(state_.center_freq_hz.load()) / 1e6,
                static_cast<float>(state_.sample_rate_hz.load()) / 1e6f,
                state_.fft_size.load(),
                state_.total_frames.load());

    if (playing) {
        ImGui::SameLine();
        ImGui::TextColored({0.4f, 0.9f, 0.4f, 1.0f}, "|  PLAYING FILE");
    }

    // предупреждения — только когда есть о чём предупредить, чтобы строка не
    // превращалась в постоянный шум
    if (!state_.audio_device_ok.load() && state_.demod_mode.load() != DemodMode::Off) {
        ImGui::SameLine();
        ImGui::TextColored({0.95f, 0.5f, 0.3f, 1.0f}, "|  audio device unavailable");
    }
    // Перегрузка входа. Стоит рядом с признаком воспроизведения не случайно:
    // на записи её уже не исправить, и знать о ней надо до того, как по этой
    // записи начнут что-то мерить.
    if (state_.clip_active.load()) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "|  CLIP %.2f%%", state_.clip_pct1.load());
    } else if (state_.clip_windows.load() > 0) {
        ImGui::SameLine();
        ImGui::TextColored({0.95f, 0.6f, 0.3f, 1.0f}, "|  clipping seen (%llu windows, worst %.2f%%)",
                            static_cast<unsigned long long>(state_.clip_windows.load()),
                            state_.clip_worst_pct.load());
    }

    uint64_t dropped = state_.save_dropped_samples.load();
    if (!state_.is_saving.load() && dropped > 0) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.45f, 0.45f, 1.0f}, "|  last recording dropped %llu samples (disk too slow)",
                            static_cast<unsigned long long>(dropped));
    }

    // Переключатели отдельных окон живут здесь, у состояния приложения: они
    // относятся не к какой-то одной секции настроек, а к тому, что показано
    // на экране.
    float btn_w = 120.0f;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - btn_w * 3.0f - ImGui::GetStyle().ItemSpacing.x * 2.0f);
    if (ImGui::Button("Peleng", ImVec2(btn_w, 0.0f)))
        active_page_ = Page::Peleng;
    ImGui::SameLine();
    if (ImGui::Button("TV picture", ImVec2(btn_w, 0.0f)))
        active_page_ = Page::Television;
    ImGui::SameLine();
    if (ImGui::Button("Phase history", ImVec2(btn_w, 0.0f)))
        active_page_ = Page::Phase;
}

void SpectrumRenderer::render_device_section() {
    // Секция всегда раскрыта: Leaf убирает стрелку и делает заголовок
    // некликабельным, поэтому настройки не могут схлопнуться под руками —
    // ни по клику, ни при смене состояния подключения.
    ImGui::CollapsingHeader("Device", ImGuiTreeNodeFlags_Leaf);

    std::vector<std::string> local_uris;
    {
        std::lock_guard<std::mutex> lock(state_.uri_list_mutex);
        local_uris = state_.found_uris;
    }
    int current_uri_idx = 0;
    std::string current_ip = state_.pluto_ip;
    for (size_t i = 0; i < local_uris.size(); ++i) {
        if (local_uris[i].rfind(current_ip, 0) == 0 || current_ip == local_uris[i]) {
            current_uri_idx = static_cast<int>(i);
            break;
        }
    }
    std::vector<const char*> combo_items;
    for (const auto& uri : local_uris)
        combo_items.push_back(uri.c_str());

    labeled_item("Device", 250.0f);
    if (ImGui::Combo("##uri", &current_uri_idx, combo_items.data(), static_cast<int>(combo_items.size()))) {
        std::lock_guard<std::mutex> lock(state_.connection_mutex);
        std::string selected_uri = local_uris[current_uri_idx];
        size_t space_pos = selected_uri.find(' ');
        state_.pluto_ip = (space_pos != std::string::npos) ? selected_uri.substr(0, space_pos) : selected_uri;
    }
    ImGui::SameLine();
    if (state_.is_scanning.load()) {
        ImGui::BeginDisabled();
        ImGui::Button("Scanning...");
        ImGui::EndDisabled();
    } else if (ImGui::Button("Scan")) {
        state_.scan_requested.store(true);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputTextWithHint("##manualip", "or type ip:192.168.2.1", ip_input_buf_, sizeof(ip_input_buf_));
    ImGui::SameLine();
    if (ImGui::Button("Use")) {
        std::string manual = ip_input_buf_;
        if (!manual.empty()) {
            if (manual.rfind("ip:", 0) != 0 && manual != "local")
                manual = "ip:" + manual;
            std::lock_guard<std::mutex> lock(state_.connection_mutex);
            state_.pluto_ip = manual;
        }
    }

    // Поиск платы по MAC. IP ей выдаёт DHCP и он меняется между загрузками,
    // поэтому найденный адрес не запоминаем, а ищем заново по постоянному MAC.
    labeled_item("Board MAC", 170.0f);
    ImGui::InputTextWithHint("##boardmac", "42:7e:d7:4e:0f:77", board_mac_buf_, sizeof(board_mac_buf_));
    ImGui::SameLine();
    if (state_.board_is_finding.load()) {
        ImGui::BeginDisabled();
        ImGui::Button("Searching...");
        ImGui::EndDisabled();
    } else if (ImGui::Button("Find board")) {
        std::lock_guard<std::mutex> lock(state_.board_mutex);
        state_.board_mac = board_mac_buf_;
        state_.board_find_requested.store(true);
    }

    std::string board_ip, board_status;
    {
        std::lock_guard<std::mutex> lock(state_.board_mutex);
        board_ip     = state_.board_found_ip;
        board_status = state_.board_status;
    }
    if (!board_ip.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Use as device")) {
            std::lock_guard<std::mutex> lock(state_.connection_mutex);
            state_.pluto_ip = "ip:" + board_ip;
        }
    }
    if (!board_status.empty())
        ImGui::TextUnformatted(board_status.c_str());

    // Настройки ниже применяются только переподключением, но навязывать его
    // отключённой плате нельзя: пользователь разорвал связь намеренно, и
    // правка поля не должна поднимать её обратно за него.
    auto reapply_if_connected = [this]() {
        if (state_.is_connected.load())
            state_.connect_requested.store(true);
    };

    labeled_item("Input", 120.0f);
    static const char* chan_modes[] = { "Rx 1", "Rx 2", "Rx 1 + Rx 2" };
    int current_mode = static_cast<int>(state_.chan_mode.load());
    if (ImGui::Combo("##channels", &current_mode, chan_modes, 3)) {
        state_.chan_mode.store(static_cast<PlutoChannelMode>(current_mode));
        reapply_if_connected();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    static const char* port_options[] = { "A_BALANCED", "B_BALANCED", "C_BALANCED", "A_N", "A_P", "B_N", "B_P" };
    int current_port = state_.rf_port_idx.load();
    if (ImGui::Combo("RF port", &current_port, port_options, 7)) {
        state_.rf_port_idx.store(current_port);
        reapply_if_connected();
    }

    // Инверсия Rx2. Отдельной строкой, а не рядом с каналами: она применяется
    // переинициализацией чипа, и её состояние надо видеть вместе с ответом платы.
    ImGui::TextUnformatted("");
    ImGui::SameLine(kLabelColumnWidth);
    bool phase_inversion = state_.rx_phase_inversion.load();
    if (ImGui::Checkbox("Invert Rx2 phase", &phase_inversion)) {
        state_.rx_phase_inversion.store(phase_inversion);
        reapply_if_connected();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("AD9361 runs Rx2 in antiphase to Rx1 by default — measured 180 deg +/- 3\n"
                          "between the channels. For peleng that is a mirrored bearing, not a small\n"
                          "correction, so this is on by default.\n"
                          "Sets adi,rx1-rx2-phase-inversion-enable; applied by re-initialising the\n"
                          "chip, so it takes a reconnect and does not survive a board reboot.");

    // Стабильная фаза: отключает адаптивные алгоритмы AD9361 (rf_dc_offset_tracking_en,
    // bb_dc_offset_tracking_en, quadrature_tracking_en) для получения стабильной фазы
    // между каналами, независимой от уровня сигнала (AM-to-PM эффект).
    ImGui::TextUnformatted("");
    ImGui::SameLine(kLabelColumnWidth);
    bool phase_stable = state_.phase_stable_mode.load();
    if (ImGui::Checkbox("Phase stable mode", &phase_stable)) {
        state_.phase_stable_mode.store(phase_stable);
        reapply_if_connected();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Disable AD9361 adaptive algorithms (RF/BB DC offset tracking,\n"
                          "quadrature tracking) for phase-stable measurements between RX1/RX2.\n"
                          "Removes AM-to-PM effect when signal level changes (RSSI variation).\n"
                          "Required for accurate phase calibration; applied at reconnect.");

    // SameLine — внутри веток, а не перед ними: в норме (плата отвечает и
    // инверсия уже применена) приписки нет, и висящий SameLine затягивал на
    // эту же строку следующую — галочка калибровки рисовалась поверх этой.
    if (state_.is_connected.load()) {
        if (!state_.rx_phase_inversion_supported.load()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(not exposed by this firmware)");
        } else if (state_.rx_phase_inversion_applied.load() != phase_inversion) {
            ImGui::SameLine();
            ImGui::TextDisabled("(reconnect to apply)");
        }
    }

    // Калибровка перекоса каналов. Стоит следом за инверсией Rx2: обе правят
    // одно и то же — разницу между трактами, только инверсия снимает штатные
    // 180 градусов чипа, а таблица остаток, измеренный генератором.
    ImGui::TextUnformatted("");
    ImGui::SameLine(kLabelColumnWidth);
    bool cal_on = state_.rx_cal_enabled.load();
    bool cal_loaded = state_.rx_cal_loaded.load();
    ImGui::BeginDisabled(!cal_loaded);
    if (ImGui::Checkbox("Apply Rx2 calibration", &cal_on))
        state_.rx_cal_enabled.store(cal_on);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        std::string status;
        {
            std::lock_guard<std::mutex> lock(state_.rx_cal_mutex);
            status = state_.rx_cal_status;
        }
        ImGui::SetTooltip("Removes the receivers' own Rx2/Rx1 phase and amplitude error,\n"
                          "measured with a generator over frequency and gain.\n"
                          "Applied to the Rx2 samples themselves, so peleng, cross-phase and\n"
                          "the spectrum all see corrected data.\n"
                          "Only in dual-channel mode, and only with manual gain equal on both\n"
                          "channels — that is how the table was measured.\n\n%s",
                          status.empty() ? "no table loaded" : status.c_str());
    }

    // Путь редактируется здесь, а не зашит: таблица платы и таблица конкретного
    // стенда — разные файлы, и переключаться между ними переименованием файлов
    // означает рано или поздно перепутать, какая из них сейчас применяется.
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##rxcalpath", "data/rx_calibration.csv",
                              rx_cal_path_buf_, sizeof(rx_cal_path_buf_));

    ImGui::SameLine();
    if (ImGui::SmallButton("Reload##rxcal")) {
        {
            std::lock_guard<std::mutex> lock(state_.rx_cal_mutex);
            state_.rx_cal_path = rx_cal_path_buf_;
        }
        state_.rx_cal_reload_requested.store(true);
    }

    if (!cal_loaded) {
        ImGui::SameLine();
        ImGui::TextDisabled("(no table)");
    } else if (cal_on) {
        // Что именно вычитается прямо сейчас. Без этих цифр галочка — вера, а
        // не измерение: за краем сетки поправка берётся с границы, и это видно.
        ImGui::SameLine();
        if (state_.chan_mode.load() != PlutoChannelMode::Both) {
            ImGui::TextDisabled("(dual-channel only)");
        } else {
            ImGui::TextDisabled("%+.2f deg  %+.2f dB%s",
                                state_.rx_cal_phase_deg.load(),
                                state_.rx_cal_amp_db.load(),
                                state_.rx_cal_in_range.load() ? "" : "  outside the table");
        }
    }

    labeled_item("Sampling", 130.0f);
    int sample_rate_mhz = static_cast<int>(state_.sample_rate_hz.load() / 1'000'000);
    if (ImGui::InputInt("MSPS##rate", &sample_rate_mhz)) {
        sample_rate_mhz = max(1, min(61, sample_rate_mhz));
        state_.sample_rate_hz.store(sample_rate_mhz * 1'000'000);
        reapply_if_connected();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Must match the signal you are receiving.\n"
                          "When playing a recording, set this to the rate it was captured at —\n"
                          "TV line timing is derived from it, and a mismatch smears the picture.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130.0f);
    int bandwidth_mhz = static_cast<int>(state_.bandwidth_hz.load() / 1'000'000);
    if (ImGui::InputInt("MHz filter", &bandwidth_mhz)) {
        bandwidth_mhz = max(1, min(56, bandwidth_mhz));
        state_.bandwidth_hz.store(bandwidth_mhz * 1'000'000);
        reapply_if_connected();
    }

    ImGui::SameLine();
    bool connected_now = state_.is_connected.load();
    ImGui::PushStyleColor(ImGuiCol_Button, connected_now ? ImVec4(0.2f, 0.5f, 0.25f, 1.0f)
                                                         : ImVec4(0.6f, 0.25f, 0.25f, 1.0f));
    if (ImGui::Button(connected_now ? "Reconnect" : "Connect"))
        state_.connect_requested.store(true);
    ImGui::PopStyleColor();

    // Отключение от платы. Кнопка неактивна, пока связи нет, — чтобы состояние
    // читалось с одного взгляда, а не проверялось нажатием.
    ImGui::SameLine();
    ImGui::BeginDisabled(!connected_now);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.45f, 0.35f, 0.15f, 1.0f));
    if (ImGui::Button("Disconnect"))
        state_.disconnect_requested.store(true);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Close the libiio connection and release the board.\n"
                          "Streaming stops; settings stay as they are.");

    // Полный снимок SPI-регистров чипа (0x000..0x3FF), а не именованных
    // debug-атрибутов вроде инверсии/initialize выше. Нужна живая связь: файл
    // пишет CaptureThread, единственный владелец phy_dev_.
    bool dump_in_progress = state_.ad9361_reg_dump_in_progress.load();
    ImGui::BeginDisabled(!connected_now || dump_in_progress);
    if (ImGui::Button(dump_in_progress ? "Dumping..." : "Dump AD9361 registers"))
        state_.ad9361_reg_dump_requested.store(true);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Reads every AD9361 SPI register (0x000..0x3FF) via iio_device_reg_read\n"
                          "and saves address,value pairs to data/ad9361_regs_<timestamp>.csv.");

    std::string reg_dump_status;
    {
        std::lock_guard<std::mutex> lock(state_.ad9361_reg_dump_mutex);
        reg_dump_status = state_.ad9361_reg_dump_status;
    }
    if (!reg_dump_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", reg_dump_status.c_str());
    }
}

void SpectrumRenderer::render_tuning_section() {
    ImGui::SeparatorText("Tuning");

    labeled_item("Frequency", 260.0f);
    float freq_mhz = static_cast<float>(state_.center_freq_hz.load()) / 1e6f;
    if (ImGui::SliderFloat("##freqslider", &freq_mhz, 0.0f, 6000.0f, "%.1f MHz")) {
        state_.center_freq_hz.store(static_cast<uint64_t>(freq_mhz * 1e6f));
        freq_axis_.clear();
        state_.peak_hold_reset.store(true);
    }
    bool freq_slider_hovered = ImGui::IsItemHovered();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    static float freq_input = freq_mhz;
    if (ImGui::InputFloat("MHz##input", &freq_input, 0.0f, 0.0f, "%.3f", ImGuiInputTextFlags_EnterReturnsTrue)) {
        freq_input = max(0.0f, min(6000.0f, freq_input));
        state_.center_freq_hz.store(static_cast<uint64_t>(freq_input * 1e6f));
        freq_axis_.clear();
        state_.peak_hold_reset.store(true);
    }
    bool freq_input_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemDeactivated())
        freq_input = static_cast<float>(state_.center_freq_hz.load()) / 1e6f;

    // Fine-tuning: a step-size picker plus arrow buttons (held-repeat) and
    // mouse wheel over the slider/input above nudge the center frequency by
    // exactly that step — the slider alone only gives ~MHz-ish resolution
    // over a 0-6000 MHz range, too coarse to zero in on a specific channel.
    ImGui::SameLine();
    static const char* kFreqStepLabels[] = { "1 MHz", "100 kHz", "10 kHz", "1 kHz" };
    static const uint64_t kFreqStepValues[] = { 1'000'000ULL, 100'000ULL, 10'000ULL, 1'000ULL };
    static int freq_step_idx = 1;
    ImGui::SetNextItemWidth(90.0f);
    ImGui::Combo("##freqstep", &freq_step_idx, kFreqStepLabels, IM_ARRAYSIZE(kFreqStepLabels));
    uint64_t freq_step_hz = kFreqStepValues[freq_step_idx];

    auto nudge_freq = [&](int direction) {
        int64_t cur  = static_cast<int64_t>(state_.center_freq_hz.load());
        int64_t next = cur + static_cast<int64_t>(direction) * static_cast<int64_t>(freq_step_hz);
        next = max<int64_t>(0, min<int64_t>(6'000'000'000LL, next));
        state_.center_freq_hz.store(static_cast<uint64_t>(next));
        freq_input = static_cast<float>(next) / 1e6f;
        freq_axis_.clear();
        state_.peak_hold_reset.store(true);
    };

    ImGui::SameLine(0.0f, 4.0f);
    ImGui::PushButtonRepeat(true);
    if (ImGui::ArrowButton("##freq_down", ImGuiDir_Left))
        nudge_freq(-1);
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::ArrowButton("##freq_up", ImGuiDir_Right))
        nudge_freq(1);
    ImGui::PopButtonRepeat();
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        ImGui::SetTooltip("Step: %s (hold to repeat, or scroll over the frequency field)", kFreqStepLabels[freq_step_idx]);

    // mouse wheel while hovering the slider or the numeric field also steps
    // by freq_step_hz — one tick per notch, no need to grab the arrows
    float wheel = ImGui::GetIO().MouseWheel;
    if ((freq_slider_hovered || freq_input_hovered) && wheel != 0.0f)
        nudge_freq(wheel > 0.0f ? 1 : -1);

    render_gain_controls();
}

// Усиление AD9361. Режим и уровень применяются на лету, без переподключения,
// поэтому здесь нет reapply_if_connected(), как у настроек из секции Device.
void SpectrumRenderer::render_gain_controls() {
    labeled_item("Gain", 150.0f);

    static const char* gain_modes[] = { "Manual", "AGC slow", "AGC fast", "Hybrid" };
    GainMode mode_now = state_.gain_mode.load();
    int current_gain_mode = static_cast<int>(mode_now);
    if (ImGui::Combo("##gainmode", &current_gain_mode, gain_modes, IM_ARRAYSIZE(gain_modes))) {
        mode_now = static_cast<GainMode>(current_gain_mode);
        state_.gain_mode.store(mode_now);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Manual   - gain index is held at the value set below\n"
                          "AGC slow - chip tracks average power, steps slowly (continuous signals)\n"
                          "AGC fast - chip reacts to overload peaks inside a burst (bursty/TDD)\n"
                          "Hybrid   - your level, but overload detectors may pull it down");

    // Уровень задаётся только там, где индекс усиления держим мы: в режимах АРУ
    // его выбирает чип, и запись hardwaregain драйвер отклоняет. Ползунок в этих
    // режимах гасим, а не прячем — иначе секция прыгает по высоте при смене
    // режима, и непонятно, куда делась ручка.
    bool manual_gain  = (mode_now == GainMode::Manual || mode_now == GainMode::Hybrid);
    PlutoChannelMode chan_mode = state_.chan_mode.load();
    bool dual         = chan_mode == PlutoChannelMode::Both;
    bool link         = state_.gain_link_channels.load();

    ImGui::SameLine();
    ImGui::BeginDisabled(!manual_gain);
    ImGui::SetNextItemWidth(200.0f);
    int gain = state_.gain_db.load();
    if (ImGui::SliderInt(dual ? "dB Rx1##gain" : "dB##gain", &gain,
                         AppState::GAIN_DB_MIN, AppState::GAIN_DB_MAX))
        state_.gain_db.store(gain);
    ImGui::EndDisabled();
    if (!manual_gain && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The AGC picks the gain index in this mode.\nSwitch to Manual or Hybrid to set it by hand.");

    if (dual) {
        ImGui::SameLine();
        if (ImGui::Checkbox("Link Rx2", &link))
            state_.gain_link_channels.store(link);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Rx2 follows Rx1. Peleng compares the two channels by amplitude,\n"
                              "so a gain difference between them biases the measurement.");

        ImGui::BeginDisabled(!manual_gain || link);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        int gain2 = link ? gain : state_.gain_db2.load();
        if (ImGui::SliderInt("dB Rx2##gain2", &gain2, AppState::GAIN_DB_MIN, AppState::GAIN_DB_MAX))
            state_.gain_db2.store(gain2);
        ImGui::EndDisabled();
    }

    // Что стоит на самом деле. В режимах АРУ это единственный способ увидеть, на
    // чём усиление остановилось; в ручном — проверка, что запрошенные дБ прошли:
    // выше 1300 МГц таблица усиления короче, и запрос сверх её максимума драйвер
    // не принимает, оставляя прежний индекс.
    if (state_.gain_readback_valid.load()) {
        ImGui::TextUnformatted("");
        ImGui::SameLine(kLabelColumnWidth);
        if (dual)
            ImGui::TextDisabled("actual  Rx1 %.0f dB (RSSI %.1f dB)   Rx2 %.0f dB (RSSI %.1f dB)",
                                state_.gain_actual_db.load(), state_.rssi_db.load(),
                                state_.gain_actual_db2.load(), state_.rssi_db2.load());
        else if (chan_mode == PlutoChannelMode::Channel_2)
            ImGui::TextDisabled("actual %.0f dB   RSSI %.1f dB",
                                state_.gain_actual_db2.load(), state_.rssi_db2.load());
        else
            ImGui::TextDisabled("actual %.0f dB   RSSI %.1f dB",
                                state_.gain_actual_db.load(), state_.rssi_db.load());
    }

    // Разность фаз между трактами. Единственное место, где расхождение каналов
    // видно до того, как оно уедет в пеленг: ~180 градусов означают, что Rx2
    // идёт в противофазе (см. галочку Invert Rx2 phase в секции Device).
    if (dual && state_.cross_phase_valid.load()) {
        float phase = state_.cross_phase_deg.load();
        float gamma = state_.cross_coherence.load();

        ImGui::TextUnformatted("");
        ImGui::SameLine(kLabelColumnWidth);
        // Группой, чтобы подсказка ловилась над всей строкой, а не только над
        // последним её куском: строка собрана из нескольких Text подряд.
        ImGui::BeginGroup();
        // Фаза считается по всей полосе сразу, поэтому на некоррелированных
        // каналах это случайное число. Порог отсекает именно такой случай:
        // печатать градусы, которых никто не измерял, хуже, чем не печатать
        // ничего. 0.1 — уверенно выше шумового пола оценки на буфере в
        // сотни тысяч отсчётов, но ниже любой реальной общей засветки.
        constexpr float kCoherenceFloor = 0.1f;
        if (gamma < kCoherenceFloor) {
            ImGui::TextDisabled("Rx1-Rx2 phase  --   (coherence %.2f: no signal common to both)", gamma);
        } else {
            bool inverted = fabsf(phase) > 90.0f;
            ImGui::TextDisabled("Rx1-Rx2 phase");
            ImGui::SameLine();
            ImGui::TextColored(inverted ? ImVec4(0.95f, 0.55f, 0.25f, 1.0f)
                                        : ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                               "%+.1f deg", phase);
            ImGui::SameLine();
            ImGui::TextDisabled("(coherence %.2f)%s", gamma,
                                inverted ? "  channels are in antiphase" : "");
        }
        ImGui::EndGroup();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("arg(sum Rx1 * conj(Rx2)) over the last buffer, and the coherence\n"
                              "that says whether to trust it. Measured across the whole band at\n"
                              "once, so it only means something when one signal dominates:\n"
                              "with the band full of uncorrelated noise the coherence stays near 0.\n\n"
                              "History and manual trim: \"Phase history\" button, top right.");
    }
}

void SpectrumRenderer::render_display_section() {
    ImGui::SeparatorText("Display");

    labeled_item("FFT size", 100.0f);
    static const char* fft_options[] = { "1024", "2048", "4096", "8192", "16384", "32768", "65536" };
    static const int   fft_values[]  = { 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
    static const int   fft_option_count = static_cast<int>(sizeof(fft_values) / sizeof(fft_values[0]));
    int current_fft = state_.fft_size.load();
    int current_idx = 0;
    for (int i = 0; i < fft_option_count; ++i)
        if (fft_values[i] == current_fft) current_idx = i;
    if (ImGui::Combo("##fftsize", &current_idx, fft_options, fft_option_count))
        state_.fft_size.store(fft_values[current_idx]);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    float cal = state_.cal_offset_db.load();
    if (ImGui::InputFloat("dB cal offset", &cal, 0.5f, 1.0f, "%.1f"))
        state_.cal_offset_db.store(cal);

    ImGui::SameLine();
    bool peak_on = state_.peak_hold_on.load();
    if (ImGui::Checkbox("Peak hold", &peak_on))
        state_.peak_hold_on.store(peak_on);
    ImGui::SameLine();
    if (ImGui::Button("Reset peak"))
        state_.peak_hold_reset.store(true);

    ImGui::SameLine();
    bool exclude_dc = state_.exclude_dc_bin.load();
    if (ImGui::Checkbox("Exclude DC", &exclude_dc))
        state_.exclude_dc_bin.store(exclude_dc);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hides the LO-leakage spike at the center bin by interpolating it from its neighbors.");

    // Числа SNR — здесь, где есть место под целую строку; на самом графике от
    // них остаются только линия шумового пола и подпись у пика.
    labeled_item("SNR", 0.0f);
    bool snr_overlay = state_.snr_overlay_on.load();
    if (ImGui::Checkbox("mark on plot", &snr_overlay))
        state_.snr_overlay_on.store(snr_overlay);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draws the noise floor line and the SNR label at the strongest bin.");

    if (state_.snr_valid.load()) {
        ImGui::SameLine();
        ImGui::Text("%s  peak %.1f dBm @ %.4f MHz   noise %.1f dBm   SNR %.1f dB",
                    state_.chan_mode.load() == PlutoChannelMode::Channel_2 ? "Rx2" : "Rx1",
                    state_.snr_peak_dbm.load(), state_.snr_peak_hz.load() / 1e6,
                    state_.snr_noise_dbm.load(), state_.snr_db.load());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Noise floor is the median over FFT bins, signal is the strongest bin, so\n"
                              "SNR here is peak-over-noise in ONE bin - the number a spectrum analyzer\n"
                              "shows. A demodulator sees less: its noise is collected over the whole\n"
                              "channel bandwidth, not one bin.");
        if (state_.snr_valid2.load()) {
            ImGui::TextDisabled("Rx2  peak %.1f dBm @ %.4f MHz   noise %.1f dBm   SNR %.1f dB",
                                state_.snr_peak_dbm2.load(), state_.snr_peak_hz2.load() / 1e6,
                                state_.snr_noise_dbm2.load(), state_.snr_db2.load());
        }
    } else {
        ImGui::SameLine();
        ImGui::TextDisabled("no spectrum yet");
    }

    if (cursor_locked_) {
        ImGui::TextColored({1.0f, 1.0f, 0.0f, 1.0f}, "Cursor %.4f MHz  %.1f dBm", cursor_freq_hz_ / 1e6f, cursor_dbm_);
        if (state_.snr_valid.load()) {
            ImGui::SameLine();
            ImGui::TextColored({1.0f, 1.0f, 0.0f, 1.0f}, " (%+.1f dB over noise)",
                                cursor_dbm_ - state_.snr_noise_dbm.load());
        }
        ImGui::SameLine();
        if (ImGui::Button("Unlock"))
            cursor_locked_ = false;
    } else {
        ImGui::TextDisabled("click the PSD plot to lock a cursor");
    }
}

void SpectrumRenderer::render_demod_section() {
    ImGui::SeparatorText("Demodulation");

    labeled_item("Mode", 120.0f);
    static const char* demod_modes[] = { "Off", "FM Audio", "TV Video" };
    DemodMode demod_mode_now = state_.demod_mode.load();
    int current_demod_mode = static_cast<int>(demod_mode_now);
    if (ImGui::Combo("##demodmode", &current_demod_mode, demod_modes, 3))
        state_.demod_mode.store(static_cast<DemodMode>(current_demod_mode));

    if (demod_mode_now == DemodMode::FmAudio) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        int fm_bw_khz = static_cast<int>(state_.fm_channel_bw_hz.load() / 1000);
        if (ImGui::InputInt("kHz channel", &fm_bw_khz)) {
            fm_bw_khz = max(1, min(15000, fm_bw_khz));
            state_.fm_channel_bw_hz.store(static_cast<uint32_t>(fm_bw_khz) * 1000);
        }
    }

    if (demod_mode_now == DemodMode::TvVideo) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90.0f);
        static const char* tv_standards[] = { "NTSC", "PAL" };
        int current_standard = static_cast<int>(state_.tv_standard.load());
        if (ImGui::Combo("##tvstandard", &current_standard, tv_standards, 2))
            state_.tv_standard.store(static_cast<TvStandard>(current_standard));

        ImGui::SameLine();
        if (ImGui::Button("Show picture"))
            active_page_ = Page::Television;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Opens the TV page, where the picture, signal diagnostics\n"
                              "and noise reduction live.");

        // Тонкая настройка ТВ убрана под свёрнутый заголовок: в норме её не
        // трогают, а раньше все эти ползунки лежали в общем потоке и создавали
        // впечатление, что для картинки надо что-то настраивать.
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::TreeNode("TV tuning")) {
            labeled_item("Sync level", 150.0f);
            float sync_thresh = state_.tv_sync_threshold.load();
            if (ImGui::SliderFloat("##syncthresh", &sync_thresh, -0.95f, -0.05f))
                state_.tv_sync_threshold.store(sync_thresh);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Level below which the signal counts as a sync pulse.\n"
                                  "Lower it if the picture rolls, raise it if it never locks.");

            labeled_item("White level", 150.0f);
            bool white_auto = state_.tv_white_auto.load();
            ImGui::BeginDisabled(white_auto);
            float white_ratio = state_.tv_white_ratio.load();
            if (ImGui::SliderFloat("##whitelevel", &white_ratio, 0.5f, 4.0f, "%.2f"))
                state_.tv_white_ratio.store(white_ratio);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("White level as a multiple of the sync-to-blanking swing.\n"
                                  "2.5 = real broadcast signal (100 IRE white, 40 IRE sync).\n"
                                  "0.85 = .bin files produced by python/mp4_to_iq.py.");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Checkbox("auto", &white_auto))
                state_.tv_white_auto.store(white_auto);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Track the white level from the brightest part of the picture.\n"
                                  "Works with any source, but a dark scene lifts the whole image.");

            labeled_item("Audio sub", 150.0f);
            float sub_offset_mhz = static_cast<float>(state_.tv_audio_subcarrier_offset_hz.load() / 1e6);
            if (ImGui::InputFloat("MHz##sub", &sub_offset_mhz, 0.1f, 0.5f, "%.2f")) {
                sub_offset_mhz = max(0.5f, min(10.0f, sub_offset_mhz));
                state_.tv_audio_subcarrier_offset_hz.store(static_cast<double>(sub_offset_mhz) * 1e6);
            }
            ImGui::SameLine();
            bool sub_on = state_.tv_audio_subcarrier_on.load();
            if (ImGui::Checkbox("enabled##sub", &sub_on))
                state_.tv_audio_subcarrier_on.store(sub_on);

            ImGui::TreePop();
        }
    }

    // Звук относится к обоим режимам демодуляции, поэтому стоит после них и
    // прячется целиком, когда демодулятор выключен — раньше громкость висела
    // на экране всегда, даже когда звука в принципе нет.
    if (demod_mode_now != DemodMode::Off) {
        labeled_item("Volume", 200.0f);
        float volume = state_.audio_volume.load();
        if (ImGui::SliderFloat("##volume", &volume, 0.0f, 1.0f))
            state_.audio_volume.store(volume);

        ImGui::SameLine();
        bool muted = state_.audio_mute.load();
        if (ImGui::Checkbox("Mute", &muted))
            state_.audio_mute.store(muted);

        ImGui::SameLine();
        bool deemph_on = state_.fm_deemphasis_on.load();
        if (ImGui::Checkbox("De-emphasis", &deemph_on))
            state_.fm_deemphasis_on.store(deemph_on);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Standard FM treble correction. Leave on unless the audio sounds dull.");
    }
}

// Двухканальные записи помечаются суффиксом _2ch в имени (см. save_iq_file):
// по содержимому и размеру раскладки неразличимы, так что имя — единственная
// подсказка, которая есть у воспроизведения.
static bool path_says_dual_channel(const char* path) {
    return std::string(path).find("_2ch") != std::string::npos;
}

void SpectrumRenderer::render_record_section() {
    ImGui::SeparatorText("Record & playback");

    labeled_item("Capture", 0.0f);
    bool is_saving = state_.is_saving.load();
    if (is_saving) ImGui::BeginDisabled();
    if (ImGui::Button(is_saving ? "Recording..." : "Save IQ to file")) {
        state_.save_requested.store(true);
        cout << "[display] IQ save requested\n";
    }
    if (is_saving) ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Writes raw IQ next to the executable, in data/.");

    // Длина записи и имя файла — рядом с кнопкой, а не в отдельной секции:
    // это те два параметра, которые смотрят прямо перед нажатием.
    ImGui::SameLine();
    static const char* kIqLengthItems[] = {
        "65536 (2^16)", "131072 (2^17)", "262144 (2^18)", "524288 (2^19)", "1048576 (2^20)"
    };
    int log2n = std::clamp(state_.save_length_log2.load(), AppState::SAVE_LOG2_MIN, AppState::SAVE_LOG2_MAX);
    int len_idx = log2n - AppState::SAVE_LOG2_MIN;
    if (is_saving) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("##iq_length", &len_idx, kIqLengthItems, IM_ARRAYSIZE(kIqLengthItems)))
        state_.save_length_log2.store(len_idx + AppState::SAVE_LOG2_MIN);
    if (is_saving) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        uint32_t rate = std::max(state_.sample_rate_hz.load(), 1u);
        ImGui::SetTooltip("Samples to record, always a power of two so the file is a whole\n"
                          "number of FFT windows. %u samples = %.1f ms at %.1f MSPS.",
                          1u << log2n,
                          1000.0 * static_cast<double>(1u << log2n) / static_cast<double>(rate),
                          static_cast<double>(rate) / 1e6);
    }

    ImGui::SameLine();
    if (is_saving) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputTextWithHint("##iq_name", "file name (auto)", save_name_buf_, sizeof(save_name_buf_))) {
        std::lock_guard<std::mutex> lock(state_.save_mutex);
        state_.save_file_name = save_name_buf_;
    }
    if (is_saving) ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Name of the file inside data/. Leave empty for the automatic\n"
                          "iq_pluto_<time>_<freq>MHz name. The .bin extension and the _2ch\n"
                          "suffix (in \"Rx 1 + Rx 2\" mode) are added if missing.");

    labeled_item("Playback", 0.0f);
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::InputTextWithHint("##playbackfile", "path to a recorded .bin to play", playback_path_buf_, sizeof(playback_path_buf_))) {
        bool now_2ch = path_says_dual_channel(playback_path_buf_);
        if (now_2ch != playback_path_was_2ch_) {
            state_.playback_dual_channel.store(now_2ch);
            playback_path_was_2ch_ = now_2ch;
        }
    }

    bool playing = state_.is_playing_back.load();
    ImGui::SameLine();
    if (playing) ImGui::BeginDisabled();
    if (ImGui::Button("Play")) {
        std::string path = playback_path_buf_;
        if (!path.empty()) {
            std::lock_guard<std::mutex> lock(state_.playback_mutex);
            state_.playback_file_path = path;
            state_.playback_requested.store(true);
        }
    }
    if (playing) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!playing) ImGui::BeginDisabled();
    if (ImGui::Button("Stop"))
        state_.playback_stop_requested.store(true);
    if (!playing) ImGui::EndDisabled();

    ImGui::SameLine();
    bool loop = state_.playback_loop.load();
    if (ImGui::Checkbox("Loop", &loop))
        state_.playback_loop.store(loop);

    // Раскладка защёлкивается на старте файла, менять её на ходу нечему —
    // отсюда и запрет во время воспроизведения
    ImGui::SameLine();
    bool dual_file = state_.playback_dual_channel.load();
    if (playing) ImGui::BeginDisabled();
    // playback_path_was_2ch_ намеренно не трогаем: он помнит, что сказало ИМЯ,
    // а не что выбрал человек. Иначе снятая вручную галочка возвращалась бы
    // обратно на первой же правке пути, в котором остался суффикс _2ch.
    if (ImGui::Checkbox("2 ch", &dual_file))
        state_.playback_dual_channel.store(dual_file);
    if (playing) ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("File holds both RX channels (i1,q1,i2,q2 per sample), as recorded in\n"
                          "\"Rx 1 + Rx 2\" mode. Set automatically for names containing _2ch.\n"
                          "Direction finding needs this - monopulse has nothing to work with\n"
                          "from a single channel.");

    // Итог по последней записи. Ставится сразу после неё, потому что убавить
    // усиление можно только пока стенд ещё собран; из готового файла зажатые
    // отсчёты не восстановить ничем.
    if (state_.save_clip_valid.load()) {
        float pct = state_.save_clip_pct.load();
        if (pct > 0.0f)
            ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f},
                                "last recording: %.2f%% of samples clipped, peak %.1f dBFS - reduce gain and record again",
                                pct, state_.save_peak_dbfs.load());
        else
            ImGui::TextDisabled("last recording: no clipping, peak %.1f dBFS", state_.save_peak_dbfs.load());
    }

    ImGui::TextDisabled("set Sampling to the rate the file was recorded at");
}

void SpectrumRenderer::render_controls() {
    render_status_bar();
    ImGui::Separator();

    // Две колонки: окно широкое (1400+), а секции узкие, и в один столбец они
    // занимали почти пол-экрана по высоте, отбирая её у графиков. Слева то,
    // что относится к приёму, справа — что делать с принятым.
    if (ImGui::BeginTable("##controls_columns", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        render_device_section();
        render_tuning_section();
        render_display_section();

        ImGui::TableNextColumn();
        render_demod_section();
        render_record_section();

        ImGui::EndTable();
    }
}

void SpectrumRenderer::copy_peleng_state() {
    lock_guard<mutex> lock(state_.peleng_mutex);
    peleng_freq_mhz_     = state_.peleng_freq_mhz;
    peleng_rho_re_       = state_.peleng_rho_re;
    peleng_rho_im_       = state_.peleng_rho_im;
    peleng_gamma2_       = state_.peleng_gamma2;
    peleng_snr_db_       = state_.peleng_snr_db;
    peleng_sigma_dbfs_   = state_.peleng_sigma_dbfs;
    peleng_valid_        = state_.peleng_valid;
    peleng_update_count_ = state_.peleng_update_count.load();
}

#if 0 // Legacy standalone GLFW window; retained only as migration reference for stage 5.
void SpectrumRenderer::open_peleng_window() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // shares the main window's GL context (font atlas / buffers can be shared),
    // but is otherwise a fully independent OS-level window
    peleng_window_ = glfwCreateWindow(900, 720, "Peleng - wideband monopulse", nullptr, window_);
    if (!peleng_window_) {
        cerr << "[display] failed to create peleng window\n";
        show_peleng_window_ = false;
        return;
    }

    glfwMakeContextCurrent(peleng_window_);
    glfwSwapInterval(1);

    peleng_imgui_ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(peleng_imgui_ctx_);
    peleng_implot_ctx_ = ImPlot::CreateContext();
    ImPlot::SetCurrentContext(peleng_implot_ctx_);

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui::GetStyle().FrameRounding  = 3.0f;

    ImGui_ImplGlfw_InitForOpenGL(peleng_window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // hand control back to the main window/context for the rest of this frame
    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);

    cout << "[display] peleng window opened\n";
}

void SpectrumRenderer::close_peleng_window() {
    if (!peleng_window_) return;

    glfwMakeContextCurrent(peleng_window_);
    ImGui::SetCurrentContext(peleng_imgui_ctx_);
    ImPlot::SetCurrentContext(peleng_implot_ctx_);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext(peleng_implot_ctx_);
    ImGui::DestroyContext(peleng_imgui_ctx_);
    peleng_implot_ctx_ = nullptr;
    peleng_imgui_ctx_  = nullptr;

    glfwDestroyWindow(peleng_window_);
    peleng_window_ = nullptr;

    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);

    cout << "[display] peleng window closed\n";
}

void SpectrumRenderer::render_peleng_frame() {
    glfwMakeContextCurrent(peleng_window_);
    ImGui::SetCurrentContext(peleng_imgui_ctx_);
    ImPlot::SetCurrentContext(peleng_implot_ctx_);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##peleng_root", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove       |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw_peleng_contents();
    ImGui::End();

    ImGui::Render();
    int fw, fh;
    glfwGetFramebufferSize(peleng_window_, &fw, &fh);
    glViewport(0, 0, fw, fh);
    glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(peleng_window_);

    // hand control back to the main window/context for the rest of the loop
    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);
}
#endif

void SpectrumRenderer::draw_peleng_source_controls() {
    bool use_network = state_.peleng_use_network.load();

    ImGui::TextUnformatted("Statistics source:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Local IQ (PL model)", !use_network))
        state_.peleng_use_network.store(false);
    ImGui::SameLine();
    if (ImGui::RadioButton("Board over TCP", use_network))
        state_.peleng_use_network.store(true);

    ImGui::SameLine();
    bool compare = state_.peleng_compare_mode.load();
    if (ImGui::Checkbox("Compare local vs board", &compare))
        state_.peleng_compare_mode.store(compare);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Runs the local PC model and the board's network statistics at the same\n"
                          "time, whichever radio button above is not chosen for the graph. The\n"
                          "local accumulator switches to the board's N_FFT/B/M so subbands line\n"
                          "up by index. The CSV log below then tags every row with a numeric\n"
                          "'source' column (0 = local, 1 = network) for offline comparison in\n"
                          "python/peleng_analysis.py.\n"
                          "Needs Rx1+Rx2 connected locally AND the board reachable over TCP.");

    if (!use_network && !compare) return;

    int port = static_cast<int>(state_.peleng_net_port.load());
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Listen port", &port, 0, 0)) {
        port = std::clamp(port, 1, 65535);
        state_.peleng_net_port.store(static_cast<uint16_t>(port));
    }

    ImGui::SameLine();
    if (state_.peleng_net_connected.load())
        ImGui::TextColored(ImVec4(0.098f, 0.620f, 0.439f, 1.0f), "board connected");
    else
        ImGui::TextColored(ImVec4(0.851f, 0.349f, 0.149f, 1.0f), "waiting for board...");
}

// categorical slots from the project palette: blue = series 1, orange = series 2, aqua = series 3
static const ImVec4 kPelengBlue  { 0.224f, 0.529f, 0.898f, 1.0f };
static const ImVec4 kPelengOrange{ 0.851f, 0.349f, 0.149f, 1.0f };
static const ImVec4 kPelengAqua  { 0.098f, 0.620f, 0.439f, 1.0f };

// Состояние частотной калибровки каналов. Своя строка, а не галочка: включается
// она вместе с общей калибровкой Rx2 в панели устройства, а здесь только видно,
// дошла ли она до пеленгатора и с какой задержкой.
void SpectrumRenderer::draw_peleng_calibration_status() {
    if (!state_.peleng_cal_active.load()) {
        ImGui::TextDisabled("C1/C2: off — enable \"Apply Rx2 calibration\" in the device panel");
        return;
    }

    float tau = state_.peleng_cal_delay_ns.load();
    ImGui::TextColored(kPelengAqua, "C1/C2: on");
    ImGui::SameLine();
    ImGui::Text("delay %+.3f ns (%+.2f deg at the band edge)", tau,
                 360.0f * 0.5f * static_cast<float>(state_.sample_rate_hz.load()) * tau * 1e-9f);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Per-bin channel calibration from data/rx_calibration.csv.\n"
            "Locally the constant part is already taken off the samples by the\n"
            "capture thread, and only the delay is applied here, per bin, before\n"
            "Sigma/Delta. Blocks arriving from the board never saw the capture\n"
            "thread, so for those the whole correction is recomputed from\n"
            "S11/S22/S12 instead.");
    }
}

int SpectrumRenderer::peleng_selected_subband() const {
    int n = static_cast<int>(peleng_freq_mhz_.size());
    if (n == 0 || !peleng_sel_valid_) return -1;

    // Ось частот монотонна после fftshift, но шаг подполосы известен только
    // через сами отсчёты — поэтому просто ищем ближайший, а не считаем индекс.
    int   best      = 0;
    float best_dist = std::abs(peleng_freq_mhz_[0] - static_cast<float>(peleng_sel_freq_mhz_));
    for (int i = 1; i < n; ++i) {
        float d = std::abs(peleng_freq_mhz_[i] - static_cast<float>(peleng_sel_freq_mhz_));
        if (d < best_dist) {
            best_dist = d;
            best      = i;
        }
    }
    return best;
}

void SpectrumRenderer::update_peleng_trail(int subband) {
    int n = static_cast<int>(peleng_rho_re_.size());
    if (subband < 0 || subband >= n) return;

    // Смена подполосы (или перестройка приёмника, меняющая её смысл) обнуляет
    // след: смешивать в одном облаке точки с разных частот бессмысленно.
    if (subband != peleng_trail_subband_) {
        peleng_trail_re_.clear();
        peleng_trail_im_.clear();
        peleng_trail_time_s_.clear();
        peleng_trail_subband_ = subband;
    }

    // Одна точка на блок статистик, а не на кадр отрисовки: иначе неподвижный
    // сигнал наращивал бы облако из копий одного и того же значения.
    if (peleng_update_count_ == peleng_trail_last_update_) return;
    peleng_trail_last_update_ = peleng_update_count_;

    peleng_trail_re_.push_back(peleng_rho_re_[subband]);
    peleng_trail_im_.push_back(peleng_rho_im_[subband]);
    // Время блока — по настенным часам окна, а не по числу отсчётов (как
    // phase_hist_time_s_ у общей фазы): блоки в сетевом режиме приходят с
    // платы своим темпом, и здесь важно только «когда отрисован», а не точная
    // привязка к сэмплам.
    peleng_trail_time_s_.push_back(static_cast<float>(ImGui::GetTime()));

    int limit = std::clamp(peleng_trail_len_, 1, kPelengTrailMax);
    if (static_cast<int>(peleng_trail_re_.size()) > limit) {
        int excess = static_cast<int>(peleng_trail_re_.size()) - limit;
        peleng_trail_re_.erase(peleng_trail_re_.begin(), peleng_trail_re_.begin() + excess);
        peleng_trail_im_.erase(peleng_trail_im_.begin(), peleng_trail_im_.begin() + excess);
        peleng_trail_time_s_.erase(peleng_trail_time_s_.begin(), peleng_trail_time_s_.begin() + excess);
    }
}

void SpectrumRenderer::draw_peleng_log_controls() {
    bool logging  = state_.peleng_is_logging.load();
    bool starting = state_.peleng_log_start_requested.load();

    ImGui::TextUnformatted("CSV log:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    if (logging) ImGui::BeginDisabled();
    ImGui::InputTextWithHint("##peleng_log_path", "data/peleng_log.csv",
                              peleng_log_path_buf_, sizeof(peleng_log_path_buf_));
    if (logging) ImGui::EndDisabled();

    ImGui::SameLine();
    if (logging || starting) {
        if (ImGui::Button("Stop##peleng_log"))
            state_.peleng_log_stop_requested.store(true);
    } else {
        if (ImGui::Button("Start##peleng_log")) {
            std::string path = peleng_log_path_buf_;
            if (!path.empty()) {
                {
                    std::lock_guard<std::mutex> lock(state_.peleng_log_mutex);
                    state_.peleng_log_path = path;
                }
                state_.peleng_log_error.store(false);
                state_.peleng_log_start_requested.store(true);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Writes every accumulated subband block to CSV, next to the executable.\n"
                              "Read it back with python/peleng_analysis.py.");
    }

    ImGui::SameLine();
    bool selected_only = state_.peleng_log_selected_only.load();
    if (logging) ImGui::BeginDisabled();
    if (ImGui::Checkbox("selected subband only", &selected_only))
        state_.peleng_log_selected_only.store(selected_only);
    if (logging) ImGui::EndDisabled();

    ImGui::SameLine();
    if (state_.peleng_log_error.load()) {
        ImGui::TextColored(kPelengOrange, "log error - check the path");
    } else if (logging) {
        ImGui::TextColored(kPelengAqua, "writing %llu rows / %llu blocks",
                            static_cast<unsigned long long>(state_.peleng_log_rows_written.load()),
                            static_cast<unsigned long long>(state_.peleng_log_blocks_written.load()));
    } else if (starting) {
        ImGui::TextDisabled("waiting for the next block...");
    } else {
        ImGui::TextDisabled("idle");
    }
}

void SpectrumRenderer::draw_peleng_contents() {
    draw_peleng_source_controls();
    draw_peleng_calibration_status();
    ImGui::Separator();

    bool use_network = state_.peleng_use_network.load();
    bool compare      = state_.peleng_compare_mode.load();

    int n = static_cast<int>(peleng_freq_mhz_.size());
    if (n == 0 || !state_.peleng_has_data.load()) {
        ImGui::TextDisabled("Waiting for subband statistics...");
        if (use_network || compare)
            ImGui::TextDisabled("Waiting for the board to connect and send accumulated PL statistics.");
        if (!use_network || compare)
            ImGui::TextDisabled("Needs Rx1+Rx2 (\"Rx 1 + Rx 2\") channel mode to be connected.");
        return;
    }

    int active = 0;
    for (uint8_t v : peleng_valid_) active += v ? 1 : 0;

    ImGui::Text("Beam: boresight, 0 deg (v1, no scanning)   |   Subbands: %d   Active: %d   Updates: %llu",
                n, active, static_cast<unsigned long long>(peleng_update_count_));
    ImGui::Text("N_FFT: %d   B: %d   M: %d", state_.peleng_fft_size.load(),
                state_.peleng_subband_size.load(), state_.peleng_accum_frames.load());

    draw_peleng_log_controls();
    ImGui::Separator();

    // Частота выбирается один раз при появлении данных — по центру полосы, —
    // а дальше держится пользователем. Перестройка приёмника уводит ось за
    // пределы выбранного значения, поэтому его приходится подтягивать обратно.
    if (!peleng_sel_valid_ ||
        peleng_sel_freq_mhz_ < peleng_freq_mhz_.front() ||
        peleng_sel_freq_mhz_ > peleng_freq_mhz_.back()) {
        peleng_sel_freq_mhz_ = 0.5 * (peleng_freq_mhz_.front() + peleng_freq_mhz_.back());
        peleng_sel_valid_    = true;
    }

    int selected = peleng_selected_subband();
    state_.peleng_selected_subband.store(selected);
    update_peleng_trail(selected);

    // Вкладки, а не пятый график в общей стопке: облако на комплексной
    // плоскости читается только в примерно квадратной области, а деление
    // высоты окна ещё на одну часть сделало бы нечитаемыми и его, и остальные.
    if (ImGui::BeginTabBar("##peleng_tabs")) {
        if (ImGui::BeginTabItem("Spectrum")) {
            draw_peleng_spectrum_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Rho plane")) {
            draw_peleng_rho_plane_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Phase history")) {
            draw_peleng_phase_history_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Table")) {
            draw_peleng_table_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void SpectrumRenderer::draw_peleng_spectrum_tab() {
    int    n      = static_cast<int>(peleng_freq_mhz_.size());
    ImVec2 avail  = ImGui::GetContentRegionAvail();
    float  plot_h = (avail.y - 3.0f * ImGui::GetStyle().ItemSpacing.y) / 4.0f;

    double freq_lo = peleng_freq_mhz_.front();
    double freq_hi = peleng_freq_mhz_.back();

    if (ImPlot::BeginPlot("Sigma power", ImVec2(avail.x, plot_h))) {
        ImPlot::SetupAxes("Frequency (MHz)", "dBFS");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_lo, freq_hi, ImPlotCond_Always);
        ImPlotSpec spec;
        spec.LineColor = kPelengBlue;
        ImPlot::PlotLine("Sigma", peleng_freq_mhz_.data(), peleng_sigma_dbfs_.data(), n, spec);
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("Monopulse ratio (rho = Delta / Sigma)", ImVec2(avail.x, plot_h))) {
        ImPlot::SetupAxes("Frequency (MHz)", "rho");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_lo, freq_hi, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -2.0, 2.0, ImPlotCond_Once);
        ImPlotSpec re_spec;
        re_spec.LineColor = kPelengBlue;
        ImPlot::PlotLine("Re(rho)", peleng_freq_mhz_.data(), peleng_rho_re_.data(), n, re_spec);
        ImPlotSpec im_spec;
        im_spec.LineColor = kPelengOrange;
        ImPlot::PlotLine("Im(rho)", peleng_freq_mhz_.data(), peleng_rho_im_.data(), n, im_spec);
        // Та же линия выбора частоты, что и на вкладке Rho plane — её можно
        // тащить прямо здесь, глядя, куда попадаешь по спектру.
        ImPlot::DragLineX(0, &peleng_sel_freq_mhz_, ImVec4(1.0f, 1.0f, 1.0f, 0.6f), 1.5f);
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("SNR", ImVec2(avail.x, plot_h))) {
        ImPlot::SetupAxes("Frequency (MHz)", "dB");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_lo, freq_hi, ImPlotCond_Always);
        ImPlotSpec spec;
        spec.LineColor = kPelengBlue;
        ImPlot::PlotLine("SNR", peleng_freq_mhz_.data(), peleng_snr_db_.data(), n, spec);
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("Coherence RX1/RX2 (gamma^2)", ImVec2(avail.x, plot_h))) {
        ImPlot::SetupAxes("Frequency (MHz)", "gamma^2");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_lo, freq_hi, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
        ImPlotSpec spec;
        spec.LineColor = kPelengAqua;
        ImPlot::PlotLine("gamma^2", peleng_freq_mhz_.data(), peleng_gamma2_.data(), n, spec);
        ImPlot::EndPlot();
    }
}

// Разбаланс трактов, вытащенный из того же rho. При alpha_left = 1 (калибровка
// нулевого положения луча) Sigma = x1+x2 и Delta = x1-x2, а rho — это МНК-проекция
// Delta на Sigma, которая при одном преобладающем сигнале совпадает с Delta/Sigma.
// Тогда rho = (1-z)/(1+z), где z = x2/x1 — комплексное отношение каналов, и
// обращение однозначно:
//
//     z = (1 - rho) / (1 + rho)
//
// |z| — амплитудный разбаланс Rx2 относительно Rx1, arg z — фазовый. Центр
// плоскости (rho = 0) отвечает z = 1, то есть полностью согласованным каналам;
// по вещественной оси уводит только разница амплитуд, по мнимой — только фаз.
//
// Разница эта полная: набег от направления прихода ПЛЮС собственный разбаланс
// трактов, и одними этими данными их не разделить. Как аппаратный разбаланс
// читается либо при известном направлении (на луче остаётся именно он), либо
// когда на оба входа подан общий сигнал через делитель.
static bool rho_to_channel_ratio(float rho_re, float rho_im, float& phase_deg, float& amp_db) {
    // rho = -1 — это нуль Sigma: каналы равны по модулю и в противофазе,
    // отношение уходит в бесконечность и точка ничего не измеряет
    float den_re = 1.0f + rho_re;
    float den_im = rho_im;
    float den2   = den_re * den_re + den_im * den_im;
    if (den2 < 1e-12f) return false;

    float num_re = 1.0f - rho_re;
    float num_im = -rho_im;
    float z_re = (num_re * den_re + num_im * den_im) / den2;
    float z_im = (num_im * den_re - num_re * den_im) / den2;

    float mag2 = z_re * z_re + z_im * z_im;
    if (mag2 < 1e-20f) return false;

    phase_deg = std::atan2(z_im, z_re) * 180.0f / PI;
    amp_db    = 10.0f * std::log10(mag2); // = 20*log10(|z|)
    return true;
}

struct ChannelImbalanceStats {
    int   n{ 0 };
    float phase_mean_deg{ 0.0f };
    float phase_std_deg { 0.0f };
    float amp_mean_db   { 0.0f };
    float amp_std_db    { 0.0f };
};

// Разброс разбаланса по следу — то, ради чего след и копится: одна точка
// говорит, какой разбаланс сейчас, а СКО по облаку — можно ли этому числу
// верить и годится ли оно как поправка калибровки.
//
// Фаза усредняется по-круговому (среднее единичных векторов, СКО по Мардиа):
// обычное среднее по градусам разваливается на переходе через +-180, где
// соседние отсчёты отличаются на доли градуса, а их численные значения — на 360.
static ChannelImbalanceStats channel_imbalance_stats(const std::vector<float>& re,
                                                      const std::vector<float>& im) {
    ChannelImbalanceStats st;
    double cs = 0.0, sn = 0.0, amp_sum = 0.0, amp_sq = 0.0;

    size_t count = std::min(re.size(), im.size());
    for (size_t i = 0; i < count; ++i) {
        float phase_deg = 0.0f, amp_db = 0.0f;
        if (!rho_to_channel_ratio(re[i], im[i], phase_deg, amp_db)) continue;
        double rad = static_cast<double>(phase_deg) * PI / 180.0;
        cs      += std::cos(rad);
        sn      += std::sin(rad);
        amp_sum += amp_db;
        amp_sq  += static_cast<double>(amp_db) * amp_db;
        ++st.n;
    }
    if (st.n == 0) return st;

    st.phase_mean_deg = static_cast<float>(std::atan2(sn, cs) * 180.0 / PI);

    // R — длина результирующего вектора, 0..1: 1 = все точки смотрят в одну
    // сторону, 0 = размазаны по кругу. На полном разбросе формула уходит в
    // бесконечность, поэтому 180 градусов — потолок, хуже уже не бывает.
    double r = std::clamp(std::sqrt(cs * cs + sn * sn) / st.n, 1e-9, 1.0);
    st.phase_std_deg = static_cast<float>(std::min(std::sqrt(-2.0 * std::log(r)) * 180.0 / PI, 180.0));

    double mean = amp_sum / st.n;
    st.amp_mean_db = static_cast<float>(mean);
    st.amp_std_db  = static_cast<float>(std::sqrt(std::max(0.0, amp_sq / st.n - mean * mean)));
    return st;
}

// rho = Delta/Sigma одной подполосы, накопленный по времени и нарисованный на
// комплексной плоскости: каждый блок статистик — точка, последняя красная.
// В отличие от пеленгационной характеристики, снятой развёрткой по углу, здесь
// цель неподвижна, а разброс облака показывает шум оценки на этой частоте:
// плотное пятно = устойчивый пеленг, размазанное — нет.
void SpectrumRenderer::draw_peleng_rho_plane_tab() {
    int n        = static_cast<int>(peleng_freq_mhz_.size());
    int selected = peleng_trail_subband_;
    if (selected < 0 || selected >= n) {
        ImGui::TextDisabled("No subband selected.");
        return;
    }

    float freq_lo = peleng_freq_mhz_.front();
    float freq_hi = peleng_freq_mhz_.back();

    ImGui::SetNextItemWidth(160.0f);
    float sel_mhz = static_cast<float>(peleng_sel_freq_mhz_);
    if (ImGui::InputFloat("MHz", &sel_mhz, 0.1f, 1.0f, "%.3f"))
        peleng_sel_freq_mhz_ = std::clamp(sel_mhz, freq_lo, freq_hi);

    ImGui::SameLine();
    ImGui::Text("subband %d of %d", selected, n);

    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Trail", &peleng_trail_len_, 1, kPelengTrailMax, "%d pts");

    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        peleng_trail_re_.clear();
        peleng_trail_im_.clear();
    }

    float rho_re = peleng_rho_re_[selected];
    float rho_im = peleng_rho_im_[selected];
    float mag    = std::sqrt(rho_re * rho_re + rho_im * rho_im);
    float arg    = std::atan2(rho_im, rho_re) * 180.0f / PI;

    ImGui::Text("rho = %+.4f %+.4fj   |rho| = %.4f   arg = %+.1f deg   gamma^2 = %.3f   SNR = %.1f dB   %s",
                rho_re, rho_im, mag, arg, peleng_gamma2_[selected], peleng_snr_db_[selected],
                peleng_valid_[selected] ? "valid" : "masked out");

    // Разбаланс каналов — то же rho, пересчитанное в отношение Rx2/Rx1: текущее
    // значение и его разброс по всему следу. Стоит рядом с самим rho, потому что
    // это его прочтение, а не отдельное измерение.
    float now_phase = 0.0f, now_amp = 0.0f;
    bool  now_ok = rho_to_channel_ratio(rho_re, rho_im, now_phase, now_amp);
    ChannelImbalanceStats imb = channel_imbalance_stats(peleng_trail_re_, peleng_trail_im_);

    if (now_ok)
        ImGui::Text("Rx2/Rx1 now:    phase %+7.2f deg      amplitude %+6.2f dB", now_phase, now_amp);
    else
        ImGui::TextDisabled("Rx2/Rx1 now:    undefined (Sigma null - channels equal and antiphase)");

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Channel ratio z = Rx2/Rx1, recovered from rho as z = (1-rho)/(1+rho)\n"
                          "(exact while one signal dominates the subband).\n"
                          "arg z is the phase imbalance, |z| the amplitude imbalance;\n"
                          "rho = 0 at the center of the plane means z = 1, perfectly matched channels.\n\n"
                          "This is the TOTAL difference: propagation phase from the angle of arrival\n"
                          "plus the receivers' own imbalance. It reads as pure hardware imbalance\n"
                          "only at a known bearing, or with one signal split into both inputs.\n"
                          "Spread over the trail says whether the number is stable enough to calibrate with;\n"
                          "it is meaningless while the subband is masked out (low SNR or low gamma^2).");

    if (imb.n > 0)
        ImGui::Text("        trail:   phase %+7.2f +/- %.2f deg   amplitude %+6.2f +/- %.2f dB   (%d pts)",
                    imb.phase_mean_deg, imb.phase_std_deg, imb.amp_mean_db, imb.amp_std_db, imb.n);
    else
        ImGui::TextDisabled("        trail:   no usable points yet");

    ImVec2 avail   = ImGui::GetContentRegionAvail();
    float  strip_h = 110.0f;
    // подпись под плоскостью тоже занимает место — без этого её срезает нижним
    // краем окна
    float  caption_h = ImGui::GetTextLineHeightWithSpacing();
    float  plane_h   = avail.y - strip_h - caption_h - 3.0f * ImGui::GetStyle().ItemSpacing.y;

    // Узкая полоска спектра над плоскостью: белая линия — выбранная частота,
    // её же можно тащить мышью, не уходя на другую вкладку.
    if (ImPlot::BeginPlot("##peleng_pick", ImVec2(avail.x, strip_h))) {
        ImPlot::SetupAxes("Frequency (MHz)", "dBFS");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_lo, freq_hi, ImPlotCond_Always);
        ImPlotSpec spec;
        spec.LineColor = kPelengBlue;
        ImPlot::PlotLine("Sigma", peleng_freq_mhz_.data(), peleng_sigma_dbfs_.data(), n, spec);
        ImPlot::DragLineX(0, &peleng_sel_freq_mhz_, ImVec4(1.0f, 1.0f, 1.0f, 0.6f), 1.5f);
        ImPlot::EndPlot();
    }

    if (plane_h < 120.0f) plane_h = 120.0f;

    char title[128];
    snprintf(title, sizeof(title), "rho at %.3f MHz (Re horizontal, Im vertical)###peleng_plane",
             static_cast<double>(peleng_freq_mhz_[selected]));

    if (ImPlot::BeginPlot(title, ImVec2(avail.x, plane_h), ImPlotFlags_Equal)) {
        ImPlot::SetupAxes("Re(rho)", "Im(rho)");
        ImPlot::SetupAxisLimits(ImAxis_X1, -2.0, 2.0, ImPlotCond_Once);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -2.0, 2.0, ImPlotCond_Once);

        // Перекрестие в нуле — ноль моноимпульсного отношения и есть
        // направление, на которое смотрит луч.
        double zero = 0.0;
        ImPlotSpec axis_spec;
        axis_spec.LineColor  = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
        axis_spec.LineWeight = 1.0f;
        axis_spec.Flags      = ImPlotItemFlags_NoLegend;
        ImPlot::PlotInfLines("##vzero", &zero, 1, axis_spec);
        axis_spec.Flags = static_cast<int>(ImPlotItemFlags_NoLegend)
                        | static_cast<int>(ImPlotInfLinesFlags_Horizontal);
        ImPlot::PlotInfLines("##hzero", &zero, 1, axis_spec);

        int trail_n = static_cast<int>(peleng_trail_re_.size());
        if (trail_n > 1) {
            ImPlotSpec trail_spec;
            trail_spec.Marker          = ImPlotMarker_Circle;
            trail_spec.MarkerSize      = 2.0f;
            trail_spec.LineColor       = kPelengBlue;
            // именно MarkerLineColor определяет цвет значка в легенде у
            // PlotScatter (у PlotLine — LineColor); без него легенда берёт
            // цвет из палитры и врёт про цвет точек
            trail_spec.MarkerLineColor = kPelengBlue;
            trail_spec.MarkerFillColor = ImVec4(kPelengBlue.x, kPelengBlue.y, kPelengBlue.z, 0.55f);
            // всё, кроме последней точки — она рисуется отдельно, красной
            ImPlot::PlotScatter("history", peleng_trail_re_.data(), peleng_trail_im_.data(),
                                 trail_n - 1, trail_spec);
        }

        if (trail_n > 0) {
            const ImVec4 red { 0.90f, 0.16f, 0.16f, 1.0f };
            ImPlotSpec now_spec;
            now_spec.Marker          = ImPlotMarker_Circle;
            now_spec.MarkerSize      = 5.0f;
            now_spec.LineColor       = red;
            now_spec.MarkerLineColor = red;
            now_spec.MarkerFillColor = red;
            ImPlot::PlotScatter("latest", &peleng_trail_re_[trail_n - 1],
                                 &peleng_trail_im_[trail_n - 1], 1, now_spec);
        }

        ImPlot::EndPlot();
    }

    ImGui::TextDisabled("%d points - tight cluster = repeatable bearing, smeared cloud = noise-dominated",
                        static_cast<int>(peleng_trail_re_.size()));
}

// Тот же Rx2/Rx1 разбаланс, что в тексте на вкладке Rho plane (rho_to_channel_ratio),
// только развёрнутый во времени — прямой аналог общего окна "Rx1-Rx2 phase
// history" (draw_phase_contents), но для одной выбранной подполосы вместо
// всего буфера сразу. Точки берутся из того же следа peleng_trail_re_/im_,
// что и на комплексной плоскости, поэтому обе вкладки всегда согласованы.
void SpectrumRenderer::draw_peleng_phase_history_tab() {
    int n        = static_cast<int>(peleng_freq_mhz_.size());
    int selected = peleng_trail_subband_;
    if (selected < 0 || selected >= n) {
        ImGui::TextDisabled("No subband selected.");
        return;
    }

    // Тот же выбор, что и на вкладке Rho plane (общий peleng_sel_freq_mhz_),
    // просто ещё двумя виджетами — по номеру подполосы, а не по частоте: и
    // числом, и ползунком, чтобы не переключаться на другую вкладку ради
    // соседней подполосы. Меняют они то же самое поле, поэтому подхватится
    // тем же кодом, что уже подтягивает peleng_trail_subband_ к нему.
    int subband_idx = selected;
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("Subband##phase_hist", &subband_idx)) {
        subband_idx = std::clamp(subband_idx, 0, n - 1);
        peleng_sel_freq_mhz_ = peleng_freq_mhz_[subband_idx];
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    if (ImGui::SliderInt("##phase_hist_subband_slider", &subband_idx, 0, n - 1, "subband %d"))
        peleng_sel_freq_mhz_ = peleng_freq_mhz_[subband_idx];

    ImGui::SameLine();
    ImGui::Text("%.3f MHz", static_cast<double>(peleng_freq_mhz_[selected]));

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderInt("Trail##phase_hist", &peleng_trail_len_, 1, kPelengTrailMax, "%d pts");

    ImGui::SameLine();
    if (ImGui::Button("Clear##phase_hist")) {
        peleng_trail_re_.clear();
        peleng_trail_im_.clear();
        peleng_trail_time_s_.clear();
    }

    // Узкие полоски спектра (Sigma и SNR) сверху — тот же приём, что и в
    // Rho plane: белая линия — выбранная подполоса, её можно тащить прямо
    // здесь, глядя, куда попадаешь по спектру, а не только по номеру/частоте
    // в полях выше.
    ImVec2 full_avail = ImGui::GetContentRegionAvail();
    float  strip_h    = 90.0f;
    float  freq_lo    = peleng_freq_mhz_.front();
    float  freq_hi    = peleng_freq_mhz_.back();

    if (ImPlot::BeginPlot("##phase_hist_pick_sigma", ImVec2(full_avail.x, strip_h))) {
        ImPlot::SetupAxes(nullptr, "dBFS", ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_lo, freq_hi, ImPlotCond_Always);
        ImPlotSpec spec;
        spec.LineColor = kPelengBlue;
        ImPlot::PlotLine("Sigma", peleng_freq_mhz_.data(), peleng_sigma_dbfs_.data(), n, spec);
        ImPlot::DragLineX(0, &peleng_sel_freq_mhz_, ImVec4(1.0f, 1.0f, 1.0f, 0.6f), 1.5f);
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("##phase_hist_pick_snr", ImVec2(full_avail.x, strip_h))) {
        ImPlot::SetupAxes("Frequency (MHz)", "SNR dB");
        ImPlot::SetupAxisLimits(ImAxis_X1, freq_lo, freq_hi, ImPlotCond_Always);
        ImPlotSpec spec;
        spec.LineColor = kPelengAqua;
        ImPlot::PlotLine("SNR", peleng_freq_mhz_.data(), peleng_snr_db_.data(), n, spec);
        ImPlot::DragLineX(0, &peleng_sel_freq_mhz_, ImVec4(1.0f, 1.0f, 1.0f, 0.6f), 1.5f);
        ImPlot::EndPlot();
    }

    int trail_n = static_cast<int>(peleng_trail_re_.size());
    if (trail_n == 0) {
        ImGui::TextDisabled("waiting for blocks...");
        return;
    }

    // Sigma-null точки (rho_to_channel_ratio возвращает false) рвут линию, а
    // не тянут её через бесконечность — ImPlot пропускает NaN как разрыв.
    peleng_phase_hist_deg_.resize(trail_n);
    for (int i = 0; i < trail_n; ++i) {
        float phase_deg = 0.0f, amp_db = 0.0f;
        peleng_phase_hist_deg_[i] = rho_to_channel_ratio(peleng_trail_re_[i], peleng_trail_im_[i], phase_deg, amp_db)
                                        ? phase_deg
                                        : std::numeric_limits<float>::quiet_NaN();
    }

    float t_end = peleng_trail_time_s_[trail_n - 1];
    peleng_phase_hist_x_.resize(trail_n);
    for (int i = 0; i < trail_n; ++i)
        peleng_phase_hist_x_[i] = peleng_trail_time_s_[i] - t_end;

    double x_min = std::min(static_cast<double>(peleng_phase_hist_x_[0]), -5.0);
    ImVec2 avail = ImGui::GetContentRegionAvail();

    ImPlot::SetNextAxesLimits(x_min, 0.0, -180.0, 180.0, ImPlotCond_Always);
    if (ImPlot::BeginPlot("##peleng_phase_hist", avail, ImPlotFlags_NoMouseText | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Time (s, 0 = now)", "deg");
        ImPlot::SetupAxisLimits(ImAxis_X1, x_min, 0.0, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -180.0, 180.0, ImPlotCond_Always);

        // Ноль — ориентир, тот же смысл, что в общем окне фазы: идеально
        // согласованные каналы дают ровную линию на нуле.
        ImPlotSpec zero_spec;
        zero_spec.LineColor = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
        float zero_x[2] = { static_cast<float>(x_min), 0.0f };
        float zero_y[2] = { 0.0f, 0.0f };
        ImPlot::PlotLine("##zero", zero_x, zero_y, 2, zero_spec);

        ImPlotSpec spec;
        spec.LineColor = kPelengBlue;
        ImPlot::PlotLine("Rx2/Rx1 phase", peleng_phase_hist_x_.data(), peleng_phase_hist_deg_.data(), trail_n, spec);
        ImPlot::EndPlot();
    }

    ImGui::TextDisabled("gaps = Sigma null in that block (channels equal and antiphase, undefined ratio)");
}

// Табличный вид тех же данных подполос, что и на вкладке Spectrum, — читать
// точные числа или найти конкретную подполосу (например, самую сильную по
// SNR) быстрее по таблице, чем на глаз с графика. Столбцы сортируются по
// клику на заголовок; клик по строке выбирает частоту через тот же
// peleng_sel_freq_mhz_, что использует drag-линия на графиках, так что выбор
// остаётся согласованным со всеми остальными вкладками.
void SpectrumRenderer::draw_peleng_table_tab() {
    int n = static_cast<int>(peleng_freq_mhz_.size());
    if (n == 0) {
        ImGui::TextDisabled("No subband data.");
        return;
    }

    ImGui::Checkbox("Valid only", &peleng_table_valid_only_);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hides subbands masked out by the SNR/gamma^2 thresholds (section 19.11 of the PDF) --\n"
                          "their rho and phase are noise, not a real reading.");

    // Порядок строк собирается заново каждый кадр из фильтра/сортировки —
    // сами вектора данных (peleng_freq_mhz_ и т.д.) не трогаются, на их
    // частотный порядок опираются остальные вкладки.
    peleng_table_rows_.clear();
    for (int i = 0; i < n; ++i) {
        if (peleng_table_valid_only_ && !peleng_valid_[i]) continue;
        peleng_table_rows_.push_back(i);
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_Sortable;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (ImGui::BeginTable("##peleng_table", 10, flags, avail)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#",                  ImGuiTableColumnFlags_DefaultSort, 0.0f, 0);
        ImGui::TableSetupColumn("Freq MHz",           ImGuiTableColumnFlags_None, 0.0f, 1);
        ImGui::TableSetupColumn("Sigma dBFS",         ImGuiTableColumnFlags_None, 0.0f, 2);
        ImGui::TableSetupColumn("SNR dB",             ImGuiTableColumnFlags_None, 0.0f, 3);
        ImGui::TableSetupColumn("gamma^2",            ImGuiTableColumnFlags_None, 0.0f, 4);
        ImGui::TableSetupColumn("|rho|",              ImGuiTableColumnFlags_None, 0.0f, 5);
        ImGui::TableSetupColumn("arg(rho) deg",       ImGuiTableColumnFlags_None, 0.0f, 6);
        ImGui::TableSetupColumn("Rx2/Rx1 phase deg",  ImGuiTableColumnFlags_None, 0.0f, 7);
        ImGui::TableSetupColumn("Rx2/Rx1 amp dB",     ImGuiTableColumnFlags_None, 0.0f, 8);
        ImGui::TableSetupColumn("Valid",              ImGuiTableColumnFlags_None, 0.0f, 9);
        ImGui::TableHeadersRow();

        // |rho|/arg(rho) и Rx2/Rx1 phase/amp не хранятся отдельными векторами
        // (как на вкладке Rho plane) — они пересчитываются из rho_re/rho_im
        // здесь же, ключом сортировки и при отрисовке строки.
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsCount > 0 && specs->SpecsDirty && !peleng_table_rows_.empty()) {
                const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
                bool asc = s.SortDirection == ImGuiSortDirection_Ascending;
                auto key = [&](int i) -> double {
                    switch (s.ColumnUserID) {
                        case 0: return i;
                        case 1: return peleng_freq_mhz_[i];
                        case 2: return peleng_sigma_dbfs_[i];
                        case 3: return peleng_snr_db_[i];
                        case 4: return peleng_gamma2_[i];
                        case 5: {
                            float re = peleng_rho_re_[i], im = peleng_rho_im_[i];
                            return std::sqrt(re * re + im * im);
                        }
                        case 6: return std::atan2(peleng_rho_im_[i], peleng_rho_re_[i]);
                        case 7:
                        case 8: {
                            float ph = 0.0f, amp = 0.0f;
                            // неопределённые (Sigma-null) точки тонут в конце сортировки,
                            // а не мешаются в середине рядом с осмысленными значениями
                            if (!rho_to_channel_ratio(peleng_rho_re_[i], peleng_rho_im_[i], ph, amp))
                                return -1e30;
                            return s.ColumnUserID == 7 ? ph : amp;
                        }
                        case 9: return peleng_valid_[i] ? 1 : 0;
                        default: return 0.0;
                    }
                };
                std::sort(peleng_table_rows_.begin(), peleng_table_rows_.end(), [&](int a, int b) {
                    double ka = key(a), kb = key(b);
                    return asc ? ka < kb : ka > kb;
                });
                specs->SpecsDirty = false;
            }
        }

        int selected = peleng_trail_subband_;
        int row_count = static_cast<int>(peleng_table_rows_.size());
        ImGuiListClipper clipper;
        clipper.Begin(row_count);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                int i = peleng_table_rows_[row];
                ImGui::TableNextRow();
                if (i == selected)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(ImVec4(kPelengBlue.x, kPelengBlue.y, kPelengBlue.z, 0.25f)));

                ImGui::TableNextColumn();
                char label[32];
                snprintf(label, sizeof(label), "%d", i);
                // строка целиком кликабельна (SpanAllColumns) — клик по любой
                // ячейке выбирает подполосу, а не только по первому столбцу
                if (ImGui::Selectable(label, i == selected,
                                       ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    peleng_sel_freq_mhz_ = peleng_freq_mhz_[i];

                ImGui::TableNextColumn();
                ImGui::Text("%.3f", static_cast<double>(peleng_freq_mhz_[i]));

                ImGui::TableNextColumn();
                ImGui::Text("%.1f", static_cast<double>(peleng_sigma_dbfs_[i]));

                ImGui::TableNextColumn();
                ImGui::Text("%.1f", static_cast<double>(peleng_snr_db_[i]));

                ImGui::TableNextColumn();
                ImGui::Text("%.3f", static_cast<double>(peleng_gamma2_[i]));

                float rho_re = peleng_rho_re_[i], rho_im = peleng_rho_im_[i];
                float mag = std::sqrt(rho_re * rho_re + rho_im * rho_im);
                float arg = std::atan2(rho_im, rho_re) * 180.0f / PI;

                ImGui::TableNextColumn();
                ImGui::Text("%.4f", static_cast<double>(mag));

                ImGui::TableNextColumn();
                ImGui::Text("%+.1f", static_cast<double>(arg));

                float ph = 0.0f, amp = 0.0f;
                bool ok = rho_to_channel_ratio(rho_re, rho_im, ph, amp);

                ImGui::TableNextColumn();
                if (ok) ImGui::Text("%+.1f", static_cast<double>(ph));
                else    ImGui::TextDisabled("--");

                ImGui::TableNextColumn();
                if (ok) ImGui::Text("%+.2f", static_cast<double>(amp));
                else    ImGui::TextDisabled("--");

                ImGui::TableNextColumn();
                if (peleng_valid_[i])
                    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "valid");
                else
                    ImGui::TextDisabled("masked");
            }
        }
        ImGui::EndTable();
    }
}

#if 0 // Legacy standalone GLFW window; retained only as migration reference for stage 5.
void SpectrumRenderer::open_tv_window() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // shares the main window's GL context, same as the peleng window
    tv_window_ = glfwCreateWindow(720, 600, "TV Video - monochrome demod", nullptr, window_);
    if (!tv_window_) {
        cerr << "[display] failed to create tv video window\n";
        show_tv_window_ = false;
        return;
    }

    glfwMakeContextCurrent(tv_window_);
    glfwSwapInterval(1);

    tv_imgui_ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(tv_imgui_ctx_);
    tv_implot_ctx_ = ImPlot::CreateContext();
    ImPlot::SetCurrentContext(tv_implot_ctx_);

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui::GetStyle().FrameRounding  = 3.0f;

    ImGui_ImplGlfw_InitForOpenGL(tv_window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // hand control back to the main window/context for the rest of this frame
    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);

    cout << "[display] tv video window opened\n";
}

void SpectrumRenderer::close_tv_window() {
    if (!tv_window_) return;

    glfwMakeContextCurrent(tv_window_);
    ImGui::SetCurrentContext(tv_imgui_ctx_);
    ImPlot::SetCurrentContext(tv_implot_ctx_);

    if (tv_texture_) {
        glDeleteTextures(1, &tv_texture_);
        tv_texture_ = 0;
        tv_texture_width_ = 0;
        tv_texture_height_ = 0;
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext(tv_implot_ctx_);
    ImGui::DestroyContext(tv_imgui_ctx_);
    tv_implot_ctx_ = nullptr;
    tv_imgui_ctx_  = nullptr;

    glfwDestroyWindow(tv_window_);
    tv_window_ = nullptr;

    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);

    cout << "[display] tv video window closed\n";
}

void SpectrumRenderer::render_tv_frame() {
    glfwMakeContextCurrent(tv_window_);
    ImGui::SetCurrentContext(tv_imgui_ctx_);
    ImPlot::SetCurrentContext(tv_implot_ctx_);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##tv_root", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove       |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw_tv_contents();
    ImGui::End();

    ImGui::Render();
    int fw, fh;
    glfwGetFramebufferSize(tv_window_, &fw, &fh);
    glViewport(0, 0, fw, fh);
    glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(tv_window_);

    // hand control back to the main window/context for the rest of the loop
    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);
}
#endif

// Измеренные декодером параметры сигнала. Смысл панели — чтобы при плохой
// картинке было видно, ЧТО не так, а не приходилось гадать. Главный случай:
// частота дискретизации не совпадает с той, на которой сделана запись — тогда
// измеренный период строки уезжает от стандартных 63.5/64 usec, вся строчная
// геометрия считается неверно и картинка расплывается.
void SpectrumRenderer::draw_tv_diagnostics() {
    float line_us  = state_.tv_diag_line_us.load();
    float field_hz = state_.tv_diag_field_hz.load();
    float sync_lvl = state_.tv_diag_sync_level.load();
    float black    = state_.tv_diag_black_level.load();
    int   captured = state_.tv_diag_lines_captured.load();
    int   rejected = state_.tv_diag_lines_rejected.load();

    if (!ImGui::CollapsingHeader("Signal diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    bool  pal      = state_.tv_standard.load() == TvStandard::PAL;
    float expected = pal ? 64.0f : 63.5f;
    bool  line_bad = line_us > 0.0f && std::fabs(line_us - expected) > 0.5f;

    ImGui::Text("Line period:");
    ImGui::SameLine();
    if (line_bad) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "%.3f us  (expected %.1f)", line_us, expected);
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f),
                           "  ^ Sample Rate likely does not match the signal/recording.");
        ImGui::TextDisabled("    Line timing is derived from Sample Rate, so a mismatch smears the picture.");
    } else {
        ImGui::Text("%.3f us  (expected %.1f)", line_us, expected);
    }

    ImGui::Text("Field rate:   %.2f Hz", field_hz);
    ImGui::SameLine();
    ImGui::TextDisabled("(NTSC 59.94 / PAL 50)");

    ImGui::Text("Levels:       sync %+.3f   blanking %+.3f   swing %.3f",
                sync_lvl, black, black - sync_lvl);
    if (black - sync_lvl < 0.05f)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "  ^ Very small swing - weak signal or wrong centre frequency.");

    ImGui::Text("Lines/field:  %d captured", captured);
    ImGui::SameLine();
    if (rejected > captured / 4)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "  %d pulses rejected (noisy sync)", rejected);
    else
        ImGui::TextDisabled("  %d rejected", rejected);

    if (state_.tv_diag_stream_gap.load())
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "Stream gaps: samples are being dropped (disk or buffer cannot keep up)");
}

void SpectrumRenderer::update_tv_texture() {
    int w = local_tv_frame_width_;
    int h = local_tv_frame_height_;
    if (w <= 0 || h <= 0 || static_cast<int>(local_tv_frame_.size()) != w * h * 3) return;

    if (tv_texture_ == 0) {
        glGenTextures(1, &tv_texture_);
        glBindTexture(GL_TEXTURE_2D, tv_texture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // true RGB now (tv_frame is 3 bytes/pixel packed R,G,B — NTSC with
        // color-burst lock, or R=G=B=Y degenerate grayscale otherwise), so
        // no swizzle trick needed here anymore
    } else {
        glBindTexture(GL_TEXTURE_2D, tv_texture_);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); // tv_frame rows aren't 4-byte aligned in general

    if (w != tv_texture_width_ || h != tv_texture_height_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, local_tv_frame_.data());
        tv_texture_width_  = w;
        tv_texture_height_ = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, local_tv_frame_.data());
    }
}

void SpectrumRenderer::draw_tv_contents() {
    // recording controls — drawn regardless of whether a frame has arrived
    // yet, since the default TV dimensions (320x240) are known up front and
    // TvRecorderThread does the (possibly slow) ffmpeg launch/pipe I/O on
    // its own thread, so this never stalls the picture or the rest of the UI
    ImGui::SetNextItemWidth(320.0f);
    ImGui::InputText("Output file", record_path_buf_, sizeof(record_path_buf_));

    bool recording = state_.tv_is_recording.load();

    ImGui::SameLine();
    if (recording) ImGui::BeginDisabled();
    if (ImGui::Button("Record")) {
        std::string path = record_path_buf_;
        if (!path.empty()) {
            std::lock_guard<std::mutex> lock(state_.tv_record_mutex);
            state_.tv_record_file_path = path;
            state_.tv_record_requested.store(true);
        }
    }
    if (recording) ImGui::EndDisabled();

    ImGui::SameLine();
    if (!recording) ImGui::BeginDisabled();
    if (ImGui::Button("Stop##record"))
        state_.tv_record_stop_requested.store(true);
    if (!recording) ImGui::EndDisabled();

    if (recording) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.3f, 0.3f, 1.0f}, "| REC  %llu frames",
                            static_cast<unsigned long long>(state_.tv_record_frames_written.load()));
    } else if (state_.tv_record_error.load()) {
        ImGui::SameLine();
        ImGui::TextColored({1.0f, 0.6f, 0.2f, 1.0f}, "| Recording failed (ffmpeg missing, bad path, or size changed mid-recording)");
    }
    ImGui::Separator();

    if (local_tv_frame_.empty() || !state_.tv_frame_has_data.load()) {
        ImGui::TextDisabled("Waiting for video frames...");
        ImGui::TextDisabled("Needs Demod = \"TV Video\" and a wide enough Sample Rate (~12-16+ MSPS) to be set.");
        return;
    }

    update_tv_texture();

    ImGui::Text("Frame: %dx%d   Updates: %llu", local_tv_frame_width_, local_tv_frame_height_,
                static_cast<unsigned long long>(tv_frame_update_count_));
    if (state_.tv_color_locked.load())
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Color: locked (NTSC)");
    else
        ImGui::TextDisabled("Color: none (PAL, or no burst lock)");

    draw_tv_diagnostics();

    // ─── Noise reduction ───────────────────────────────────────────────────
    bool repair = state_.tv_repair_bad_lines.load();
    if (ImGui::Checkbox("Repair bad lines", &repair))
        state_.tv_repair_bad_lines.store(repair);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replace a line that differs sharply from both neighbours with their\n"
                          "average — impulse noise wipes out whole lines at a time.");

    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    float nr = state_.tv_temporal_nr.load();
    if (ImGui::SliderFloat("Temporal NR", &nr, 0.0f, 0.9f, "%.2f"))
        state_.tv_temporal_nr.store(nr);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blend in the previous frame where the picture is still.\n"
                          "Cleans up noise on static scenes; moving areas are left alone\n"
                          "so nothing smears behind them.");

    ImGui::Separator();

    if (tv_texture_ == 0) return;

    ImVec2 avail = ImGui::GetContentRegionAvail();
    // preserve the frame's aspect ratio within the available space
    float aspect = static_cast<float>(local_tv_frame_width_) / static_cast<float>(local_tv_frame_height_);
    float w = avail.x;
    float h = w / aspect;
    if (h > avail.y) {
        h = avail.y;
        w = h * aspect;
    }

    // tv_texture_ is a GL texture name (a plain integer handle, not a
    // pointer) — ImTextureID is ImU64 in this ImGui version, so a widening
    // static_cast is the correct conversion here (no uintptr_t needed since
    // there's no pointer being reinterpreted).
    ImGui::Image(static_cast<ImTextureID>(tv_texture_), ImVec2(w, h));
}

#if 0 // Legacy standalone GLFW window; retained only as migration reference for stage 5.
void SpectrumRenderer::open_phase_window() {
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    phase_window_ = glfwCreateWindow(640, 420, "Rx1-Rx2 phase history", nullptr, window_);
    if (!phase_window_) {
        cerr << "[display] failed to create phase history window\n";
        show_phase_window_ = false;
        return;
    }

    glfwMakeContextCurrent(phase_window_);
    glfwSwapInterval(1);

    phase_imgui_ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(phase_imgui_ctx_);
    phase_implot_ctx_ = ImPlot::CreateContext();
    ImPlot::SetCurrentContext(phase_implot_ctx_);

    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 0.0f;
    ImGui::GetStyle().FrameRounding  = 3.0f;

    ImGui_ImplGlfw_InitForOpenGL(phase_window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // hand control back to the main window/context for the rest of this frame
    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);

    cout << "[display] phase history window opened\n";
}

void SpectrumRenderer::close_phase_window() {
    if (!phase_window_) return;

    glfwMakeContextCurrent(phase_window_);
    ImGui::SetCurrentContext(phase_imgui_ctx_);
    ImPlot::SetCurrentContext(phase_implot_ctx_);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext(phase_implot_ctx_);
    ImGui::DestroyContext(phase_imgui_ctx_);
    phase_implot_ctx_ = nullptr;
    phase_imgui_ctx_  = nullptr;

    glfwDestroyWindow(phase_window_);
    phase_window_ = nullptr;

    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);

    cout << "[display] phase history window closed\n";
}

void SpectrumRenderer::render_phase_frame() {
    glfwMakeContextCurrent(phase_window_);
    ImGui::SetCurrentContext(phase_imgui_ctx_);
    ImPlot::SetCurrentContext(phase_implot_ctx_);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##phase_root", nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoMove       |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw_phase_contents();
    ImGui::End();

    ImGui::Render();
    int fw, fh;
    glfwGetFramebufferSize(phase_window_, &fw, &fh);
    glViewport(0, 0, fw, fh);
    glClearColor(0.08f, 0.08f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(phase_window_);

    // hand control back to the main window/context for the rest of the loop
    ImPlot::SetCurrentContext(main_implot_ctx_);
    ImGui::SetCurrentContext(main_imgui_ctx_);
    glfwMakeContextCurrent(window_);
}
#endif

void SpectrumRenderer::draw_phase_contents() {
    // Подстройка сверху, постоянной высоты; график ниже забирает всё, что
    // осталось от окна, и тянется вместе с ним при ресайзе.
    float trim = state_.manual_phase_trim_deg.load();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::InputFloat("deg manual trim", &trim, 0.5f, 5.0f, "%+.1f"))
        state_.manual_phase_trim_deg.store(trim);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Extra Rx2 phase rotation, independent of the calibration table in\n"
                          "the device panel and not saved anywhere. Adds directly to the\n"
                          "Rx1-Rx2 phase reading: to bring that reading to 0, enter it here\n"
                          "with the opposite sign.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset"))
        state_.manual_phase_trim_deg.store(0.0f);

    if (state_.cross_phase_valid.load()) {
        ImGui::SameLine();
        ImGui::TextDisabled("|  %+.1f deg  (coherence %.2f)",
                            state_.cross_phase_deg.load(), state_.cross_coherence.load());
    }

    ImGui::Separator();

    int hn = static_cast<int>(local_phase_hist_deg_.size());
    if (hn == 0) {
        ImGui::TextDisabled("waiting for samples...");
        return;
    }

    float t_end = local_phase_hist_time_s_[hn - 1];
    phase_hist_x_.resize(hn);
    for (int i = 0; i < hn; ++i)
        phase_hist_x_[i] = local_phase_hist_time_s_[i] - t_end;

    double x_min = std::min(static_cast<double>(phase_hist_x_[0]), -5.0);
    ImVec2 avail = ImGui::GetContentRegionAvail();

    ImPlot::SetNextAxesLimits(x_min, 0.0, -180.0, 180.0, ImPlotCond_Always);
    if (ImPlot::BeginPlot("##phase_hist", avail, ImPlotFlags_NoMouseText | ImPlotFlags_NoInputs | ImPlotFlags_NoLegend)) {
        ImPlot::SetupAxes("Time (s, 0 = now)", "deg");
        ImPlot::SetupAxisLimits(ImAxis_X1, x_min, 0.0, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, -180.0, 180.0, ImPlotCond_Always);

        // Ноль — ориентир: к нему подстройка должна сводить кривую.
        ImPlotSpec zero_spec;
        zero_spec.LineColor = ImVec4(1.0f, 1.0f, 1.0f, 0.25f);
        float zero_x[2] = { static_cast<float>(x_min), 0.0f };
        float zero_y[2] = { 0.0f, 0.0f };
        ImPlot::PlotLine("##zero", zero_x, zero_y, 2, zero_spec);

        ImPlotSpec spec;
        spec.LineColor = ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
        ImPlot::PlotLine("phase", phase_hist_x_.data(), local_phase_hist_deg_.data(), hn, spec);
        ImPlot::EndPlot();
    }
}
