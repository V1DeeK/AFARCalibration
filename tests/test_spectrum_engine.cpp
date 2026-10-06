#include <catch2/catch_test_macros.hpp>

#include "spectrum_engine.hpp"
#include "paths.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

TEST_CASE("SpectrumEngine can be started and stopped twice without hardware") {
    SpectrumEngine engine(false);

    for (int cycle = 0; cycle < 2; ++cycle) {
        engine.start();
        REQUIRE(engine.isRunning());

        engine.start();
        REQUIRE(engine.isRunning());

        engine.stopAndJoin();
        REQUIRE_FALSE(engine.isRunning());
    }
}

TEST_CASE("Spectrum user data is isolated from the executable directory") {
    const auto path = resolve_data_path("data/stage6-probe.bin");
    REQUIRE(path.parent_path() == spectrum_data_dir());
    REQUIRE(path.parent_path() != executable_dir());
}

TEST_CASE("SpectrumEngine stops while offline IQ playback is active") {
    const auto path = std::filesystem::temp_directory_path() / "afar_spectrum_lifecycle_iq.bin";
    const std::vector<std::int16_t> iq(4096 * 2, 0);
    {
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.write(reinterpret_cast<const char*>(iq.data()),
                             static_cast<std::streamsize>(iq.size() * sizeof(iq.front()))));
    }

    SpectrumEngine engine(false);
    engine.state().sample_rate_hz.store(48'000);
    engine.state().playback_loop.store(true);
    {
        std::lock_guard<std::mutex> lock(engine.state().playback_mutex);
        engine.state().playback_file_path = path.string();
    }
    engine.state().playback_requested.store(true);
    engine.start();

    for (int attempt = 0; attempt < 50 && !engine.state().is_playing_back.load(); ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(engine.state().is_playing_back.load());

    engine.requestStop();
    engine.stopAndJoin();
    REQUIRE_FALSE(engine.isRunning());

    std::error_code error;
    std::filesystem::remove(path, error);
}
