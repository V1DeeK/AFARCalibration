// Кадр платы ППКХ4 (вкладка «Настройка 1 канала»): совпадение с исходной
// страницей V1DeeK/1Chanel и контрактом docs/contracts/ppkh4-usb-cdc.md.
#include "Ppkh4Frame.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace afar::ppkh4;

TEST_CASE("ППКХ4: кадр по умолчанию — 12 бит, аттенюация 11…6, фаза 5…0", "[ppkh4]")
{
    const FrameFormat f;
    CHECK(send_command(f, 0, 0) == "SEND 12 000");
    CHECK(send_command(f, 0, 63) == "SEND 12 FC0");
    CHECK(send_command(f, 63, 0) == "SEND 12 03F");
    CHECK(send_command(f, 1, 1) == "SEND 12 041");
    CHECK(frame_hex(build_frame(f, 63, 63)) == "FFF");
}

TEST_CASE("ППКХ4: шаги фазы и аттенюации", "[ppkh4]")
{
    const FrameFormat f;
    CHECK(phase_deg(1, f.phase) == Catch::Approx(5.625));
    CHECK(phase_deg(63, f.phase) == Catch::Approx(354.375));
    CHECK(atten_db(0, f.atten) == Catch::Approx(-31.5));
    CHECK(atten_db(63, f.atten) == Catch::Approx(0.0));
    CHECK(atten_code_from_db(-10.0, f.atten) == 43);
    CHECK(atten_code_from_db(10.0, f.atten) == 43);
    CHECK(atten_code_from_db(-100.0, f.atten) == 0);
    CHECK(phase_code_from_deg(11.25, f.phase) == 2);
    CHECK(phase_code_from_deg(1000.0, f.phase) == 63);
    CHECK(clamp_code(-3, f.phase) == 0);
    CHECK(clamp_code(64, f.phase) == 63);
}

TEST_CASE("ППКХ4: инверсия, обратный порядок и фиксированные биты", "[ppkh4]")
{
    FrameFormat f;
    f.phase.reverse = true;
    CHECK(send_command(f, 1, 0) == "SEND 12 020");
    f.phase.reverse = false;
    f.atten.invert = true;
    CHECK(send_command(f, 0, 63) == "SEND 12 000");
    f.atten.invert = false;
    f.total_bits = 16;
    f.fixed_hex = "0xA000";
    CHECK(send_command(f, 0, 0) == "SEND 16 A000");
    f.fixed_hex = "zz";
    CHECK(send_command(f, 0, 0) == "SEND 16 0000");
    f.total_bits = 13;
    f.fixed_hex = "FFFF";
    CHECK(send_command(f, 0, 0) == "SEND 13 1000");
}

TEST_CASE("ППКХ4: проверки раскладки", "[ppkh4]")
{
    FrameFormat f;
    CHECK_FALSE(fields_overlap(f));
    CHECK_FALSE(fields_outside(f));
    f.atten.offset = 4;
    CHECK(fields_overlap(f));
    f.atten.offset = 8;
    CHECK(fields_outside(f));
    f.total_bits = kMaxFrameBits + 1;
    CHECK_FALSE(format_valid(f));
    f.total_bits = kMaxFrameBits;
    CHECK(format_valid(f));
    CHECK(frame_hex(build_frame(f, 0, 0)).size() == kMaxFrameBits / 4);
}

TEST_CASE("ППКХ4: команды CFG в порядке исходной страницы", "[ppkh4]")
{
    LineConfig l;
    l.half_us = 5;
    const auto cmds = cfg_commands(l);
    REQUIRE(cmds.size() == 6);
    CHECK(cmds[0] == "CFG LSBFIRST 0");
    CHECK(cmds[1] == "CFG CLKIDLE 0");
    CHECK(cmds[2] == "CFG LEACT 1");
    CHECK(cmds[3] == "CFG LEMODE 0");
    CHECK(cmds[4] == "CFG HALFUS 5");
    CHECK(cmds[5] == "CFG LEUS 1");
}

TEST_CASE("ППКХ4: перебор фазы и аттенюации вместе", "[ppkh4][sweep]")
{
    const FrameFormat f;
    using P = std::pair<int, int>;

    const auto phase = sweep_points(SweepMode::Phase, {0, 4, 2}, {0, 63, 1}, 7, 30, f);
    CHECK(phase == std::vector<P>{{0, 30}, {2, 30}, {4, 30}});

    const auto atten = sweep_points(SweepMode::Atten, {0, 63, 1}, {5, 3, 1}, 7, 30, f);
    CHECK(atten == std::vector<P>{{7, 5}, {7, 4}, {7, 3}});

    const auto grid = sweep_points(SweepMode::Grid, {0, 2, 1}, {10, 11, 1}, 0, 0, f);
    CHECK(grid == std::vector<P>{{0, 10}, {1, 10}, {2, 10}, {0, 11}, {1, 11}, {2, 11}});
    CHECK(sweep_points(SweepMode::Grid, {0, 63, 1}, {0, 63, 1}, 0, 0, f).size() == 64 * 64);

    const auto together = sweep_points(SweepMode::Together, {0, 10, 5}, {63, 0, 1}, 0, 0, f);
    CHECK(together == std::vector<P>{{0, 63}, {5, 62}, {10, 61}});

    // Коды выше ширины поля обрезаются, как в одиночном переборе.
    const auto clipped = sweep_points(SweepMode::Phase, {62, 65, 1}, {0, 0, 1}, 0, 0, f);
    CHECK(clipped == std::vector<P>{{62, 0}, {63, 0}, {63, 0}, {63, 0}});
}
