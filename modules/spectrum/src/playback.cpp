#include "playback.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

IqPlaybackThread::IqPlaybackThread(CircularBuffer<CoherentSample>& fft_buf,
                                    CircularBuffer<CoherentSample>& df_buf,
                                    CircularBuffer<CoherentSample>& demod_buf,
                                    AppState& state)
    : clip_monitor_(state), fft_buf_(fft_buf), df_buf_(df_buf), demod_buf_(demod_buf), state_(state) {}

void IqPlaybackThread::run() {
    std::cout << "[playback] started\n";

    while (state_.running.load()) {
        if (!state_.playback_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        state_.playback_requested.store(false);
        state_.playback_stop_requested.store(false);

        std::string path;
        {
            std::lock_guard<std::mutex> lock(state_.playback_mutex);
            path = state_.playback_file_path;
        }

        play_file(path);
        state_.is_playing_back.store(false);
    }

    std::cout << "[playback] exiting\n";
}

void IqPlaybackThread::play_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "[playback] failed to open " << path << "\n";
        return;
    }

    // Раскладка защёлкивается на весь файл: переключение галочки на ходу сдвинуло
    // бы разбор на половину отсчёта и рассинхронизировало каналы между собой.
    bool        dual        = state_.playback_dual_channel.load();
    std::size_t frame_bytes = dual ? 8 : 4; // i1,q1[,i2,q2] по int16

    f.seekg(0, std::ios::end);
    std::streamoff size = f.tellg();
    f.seekg(0);
    // Единственная доступная проверка догадки о числе каналов: содержимое файла
    // одинаково правдоподобно в обеих раскладках, а вот длина в отсчёты обязана
    // делиться нацело.
    if (size > 0 && static_cast<std::size_t>(size) % frame_bytes != 0)
        std::cerr << "[playback] " << path << ": size " << size << " bytes is not a multiple of "
                   << frame_bytes << " - wrong channel count selected for this file?\n";

    state_.is_playing_back.store(true);
    clip_monitor_.begin();
    std::cout << "[playback] playing " << path << (dual ? " (2 channels)" : " (1 channel)") << "\n";

    do {
        f.clear();
        f.seekg(0);
        stream_once(f, dual);
    } while (state_.playback_loop.load() && !state_.playback_stop_requested.load() && state_.running.load());

    clip_monitor_.end();
    std::cout << "[playback] finished " << path << "\n";
}

void IqPlaybackThread::stream_once(std::ifstream& f, bool dual) {
    std::size_t                 words = dual ? 4u : 2u; // int16 на отсчёт
    std::vector<int16_t>        raw(kChunkSamples * words);
    std::vector<CoherentSample> chunk(kChunkSamples);

    while (state_.running.load() && !state_.playback_stop_requested.load()) {
        auto t_start = std::chrono::steady_clock::now();

        f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * sizeof(int16_t)));
        std::streamsize got = f.gcount();
        std::size_t     n   = static_cast<std::size_t>(got) / (words * sizeof(int16_t));
        if (n == 0) break; // EOF

        // 2048 = полная шкала 12-битного АЦП AD9361, та же нормировка, что и в
        // PlutoCaptureThread — иначе уровни на записи и на живом потоке разошлись бы
        for (std::size_t i = 0; i < n; ++i) {
            const int16_t* w = &raw[i * words];
            if (dual) clip_monitor_.add(w[0], w[1], w[2], w[3]);
            else      clip_monitor_.add(w[0], w[1]);
            chunk[i].rx1 = Complex(static_cast<float>(w[0]) / 2048.0f, static_cast<float>(w[1]) / 2048.0f);
            chunk[i].rx2 = dual ? Complex(static_cast<float>(w[2]) / 2048.0f, static_cast<float>(w[3]) / 2048.0f)
                                 : Complex(0.0f, 0.0f);
        }
        clip_monitor_.tick();

        for (std::size_t i = 0; i < n; ++i) {
            fft_buf_.push(chunk[i]);
            df_buf_.push(chunk[i]);
            demod_buf_.push(chunk[i]);
        }

        // pace to roughly real-time playback against the currently
        // configured sample rate, so downstream consumers (PSD/peleng/demod
        // timing assumptions) see a realistic cadence
        uint32_t rate = std::max<uint32_t>(state_.sample_rate_hz.load(), 1);
        auto     target = std::chrono::duration<double>(static_cast<double>(n) / static_cast<double>(rate));
        auto     elapsed = std::chrono::steady_clock::now() - t_start;
        if (elapsed < target)
            std::this_thread::sleep_for(target - elapsed);
    }
}

