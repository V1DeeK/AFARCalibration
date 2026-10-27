// Файл состояния стенда .afarstate (docs/contracts/data-formats.md, «Состояние стенда»).
#include "Ppkh4State.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace afar::ppkh4;

TEST_CASE("Состояние стенда: сохранение и чтение без потерь", "[ppkh4][state]")
{
    BenchState st;
    st.name = "стенд 1";
    st.saved_at = "2026-10-27T11:00:00";
    st.s2vna_state_file = "стенд 1.sta";
    st.channel.format.total_bits = 16;
    st.channel.format.fixed_hex = "A000";
    st.channel.format.phase = {2, 5, 11.25, true, false};
    st.channel.format.atten = {8, 6, 0.5, false, true};
    st.channel.lines = {1, 1, 0, 1, 5, 7};
    st.channel.phase_code = 9;
    st.channel.atten_code = 41;
    st.channel.sweep_mode = SweepMode::Grid;
    st.channel.sweep_phase = {0, 31, 2};
    st.channel.sweep_atten = {63, 0, 3};
    st.channel.sweep_dwell_ms = 800;

    BenchState back;
    std::string error;
    REQUIRE(from_json_text(to_json_text(st), back, &error));
    CHECK(back.name == st.name);
    CHECK(back.saved_at == st.saved_at);
    CHECK(back.s2vna_state_file == st.s2vna_state_file);
    const auto& c = back.channel;
    CHECK(c.format.total_bits == 16);
    CHECK(c.format.fixed_hex == "A000");
    CHECK(c.format.phase.offset == 2);
    CHECK(c.format.phase.width == 5);
    CHECK(c.format.phase.lsb == Catch::Approx(11.25));
    CHECK(c.format.phase.invert);
    CHECK(c.format.atten.reverse);
    CHECK(c.lines.half_us == 5);
    CHECK(c.lines.le_us == 7);
    CHECK(c.lines.le_active == 0);
    CHECK(c.phase_code == 9);
    CHECK(c.atten_code == 41);
    CHECK(c.sweep_mode == SweepMode::Grid);
    CHECK(c.sweep_atten.from == 63);
    CHECK(c.sweep_atten.step == 3);
    CHECK(c.sweep_dwell_ms == 800);
    CHECK(send_command(c.format, c.phase_code, c.atten_code)
          == send_command(st.channel.format, st.channel.phase_code, st.channel.atten_code));
}

TEST_CASE("Состояние стенда: неверные файлы не принимаются", "[ppkh4][state]")
{
    BenchState st;
    std::string error;
    CHECK_FALSE(from_json_text("не json", st, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(from_json_text(R"({"format":"other","version":1})", st));

    BenchState good;
    std::string text = to_json_text(good);
    BenchState parsed;
    REQUIRE(from_json_text(text, parsed));

    // Имя .sta — только рядом с файлом состояния, без каталогов.
    good.s2vna_state_file = "../x.sta";
    CHECK_FALSE(from_json_text(to_json_text(good), parsed, &error));

    good.s2vna_state_file.clear();
    good.channel.format.total_bits = kMaxFrameBits + 1;
    CHECK_FALSE(from_json_text(to_json_text(good), parsed, &error));

    // Код больше ширины поля обрезается.
    good.channel.format = FrameFormat{};
    good.channel.phase_code = 500;
    REQUIRE(from_json_text(to_json_text(good), parsed));
    CHECK(parsed.channel.phase_code == 63);
}
