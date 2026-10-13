#include "clip_monitor.hpp"

#include <algorithm>
#include <cmath>

namespace {

// Пик в кодах АЦП → dBFS. Пустое окно (пик 0) — это не «минус бесконечность»,
// а «нечего показывать»: отдаём тот же пол, что стоит в AppState по умолчанию.
float codes_to_dbfs(int peak) {
    if (peak <= 0) return -120.0f;
    return 20.0f * std::log10(static_cast<float>(peak) / ClipMonitor::kFullScale);
}

} // namespace

void ClipMonitor::begin() {
    reset_stats();
    state_.clip_stream_active.store(true);
}

void ClipMonitor::end() {
    if (samples_ > 0) publish();
    state_.clip_stream_active.store(false);
    state_.clip_active.store(false);
}

void ClipMonitor::tick() {
    if (state_.clip_reset_requested.exchange(false)) reset_stats();

    uint32_t rate = std::max<uint32_t>(state_.sample_rate_hz.load(), 1);
    uint64_t window = static_cast<uint64_t>(kWindowSeconds * static_cast<double>(rate));
    window = std::clamp<uint64_t>(window, 1024, 1ull << 24);

    if (samples_ >= window) publish();
}

void ClipMonitor::publish() {
    if (samples_ == 0) return;

    uint32_t rate = std::max<uint32_t>(state_.sample_rate_hz.load(), 1);
    stream_time_s_ += static_cast<double>(samples_) / static_cast<double>(rate);

    float pct1 = 100.0f * static_cast<float>(ch1_.clipped) / static_cast<float>(samples_);
    float pct2 = dual_ ? 100.0f * static_cast<float>(ch2_.clipped) / static_cast<float>(samples_) : 0.0f;
    float worst_now = std::max(pct1, pct2);

    state_.clip_pct1.store(pct1);
    state_.clip_pct2.store(pct2);
    state_.clip_peak_dbfs1.store(codes_to_dbfs(ch1_.peak));
    state_.clip_peak_dbfs2.store(dual_ ? codes_to_dbfs(ch2_.peak) : -120.0f);
    state_.clip_active.store(worst_now > 0.0f);
    state_.clip_stream_time_s.store(stream_time_s_);
    state_.clip_windows_total.fetch_add(1);

    if (worst_now > 0.0f) {
        state_.clip_windows.fetch_add(1);
        state_.clip_last_time_s.store(stream_time_s_);
        if (worst_now > state_.clip_worst_pct.load()) state_.clip_worst_pct.store(worst_now);
    }

    {
        std::lock_guard<std::mutex> lock(state_.clip_mutex);
        state_.clip_hist_time_s.push_back(static_cast<float>(stream_time_s_));
        state_.clip_hist_pct1.push_back(pct1);
        state_.clip_hist_pct2.push_back(pct2);
        state_.clip_hist_dbfs1.push_back(codes_to_dbfs(ch1_.peak));
        state_.clip_hist_dbfs2.push_back(dual_ ? codes_to_dbfs(ch2_.peak) : -120.0f);

        // История — окно последних CLIP_HISTORY_LEN окон. Сдвиг стоит одного
        // memmove на 300 float раз в 100 мс, поэтому кольцевой буфер здесь
        // ничего бы не выиграл, зато читателю (UI) пришлось бы разворачивать.
        if (static_cast<int>(state_.clip_hist_time_s.size()) > AppState::CLIP_HISTORY_LEN) {
            auto drop_front = [](std::vector<float>& v) { v.erase(v.begin()); };
            drop_front(state_.clip_hist_time_s);
            drop_front(state_.clip_hist_pct1);
            drop_front(state_.clip_hist_pct2);
            drop_front(state_.clip_hist_dbfs1);
            drop_front(state_.clip_hist_dbfs2);
        }
    }

    ch1_     = Channel{};
    ch2_     = Channel{};
    samples_ = 0;
}

void ClipMonitor::reset_stats() {
    ch1_           = Channel{};
    ch2_           = Channel{};
    dual_          = false;
    samples_       = 0;
    stream_time_s_ = 0.0;

    state_.clip_active.store(false);
    state_.clip_pct1.store(0.0f);
    state_.clip_pct2.store(0.0f);
    state_.clip_peak_dbfs1.store(-120.0f);
    state_.clip_peak_dbfs2.store(-120.0f);
    state_.clip_worst_pct.store(0.0f);
    state_.clip_windows.store(0);
    state_.clip_windows_total.store(0);
    state_.clip_last_time_s.store(-1.0);
    state_.clip_stream_time_s.store(0.0);

    std::lock_guard<std::mutex> lock(state_.clip_mutex);
    state_.clip_hist_time_s.clear();
    state_.clip_hist_pct1.clear();
    state_.clip_hist_pct2.clear();
    state_.clip_hist_dbfs1.clear();
    state_.clip_hist_dbfs2.clear();
}

void ClipScan::scan(const int16_t* words, std::size_t n_samples, std::size_t stride_words) {
    for (std::size_t s = 0; s < n_samples; ++s) {
        const int16_t* w = words + s * stride_words;
        bool hit = false;
        for (std::size_t k = 0; k < stride_words; ++k) {
            int a = std::abs(static_cast<int>(w[k]));
            if (a > peak) peak = a;
            if (a >= ClipMonitor::kClipCode) hit = true;
        }
        if (hit) ++clipped;
    }
    total += n_samples;
}

float ClipScan::peak_dbfs() const {
    return codes_to_dbfs(peak);
}
