#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "DutSimulator.h"
#include "StubDutController.h"
#include "VnaSimulator.h"

#include <cmath>
#include <string>

using Catch::Matchers::ContainsSubstring;

TEST_CASE("AT-01 identify contains C2220", "[simulators]")
{
    VnaSimulator vna;
    vna.connect();
    const auto idn = vna.identify();
    REQUIRE_THAT(idn, ContainsSubstring("C2220"));
    REQUIRE_THAT(idn, ContainsSubstring("PLANAR"));
}

TEST_CASE("foreign model IDN is configurable", "[simulators]")
{
    VnaSimulator vna;
    vna.connect();
    vna.set_failure_mode(VnaSimulator::FailureMode::WrongModel);
    const auto idn = vna.identify();
    REQUIRE_THAT(idn, !ContainsSubstring("C2220"));

    vna.set_failure_mode(VnaSimulator::FailureMode::None);
    vna.set_identify_string("ACME,FAKE-VNA,1,0");
    REQUIRE_THAT(vna.identify(), ContainsSubstring("FAKE-VNA"));
}

TEST_CASE("measure_s21 returns linear frequency axis and finite S21", "[simulators]")
{
    VnaSimulator vna;
    vna.connect();
    SweepConfig cfg{};
    cfg.f_start_hz = 1'000'000'000ULL;
    cfg.f_stop_hz = 2'000'000'000ULL;
    cfg.points = 5;
    cfg.power_dbm = -10.0;
    cfg.ifbw_hz = 1000;
    cfg.averages = 1;
    vna.configure(cfg);

    const auto sweep = vna.measure_s21();
    REQUIRE(sweep.frequency_hz.size() == cfg.points);
    REQUIRE(sweep.s21.size() == cfg.points);
    REQUIRE_FALSE(sweep.overload);
    REQUIRE(sweep.frequency_hz.front() == cfg.f_start_hz);
    REQUIRE(sweep.frequency_hz.back() == cfg.f_stop_hz);
    for (const auto& z : sweep.s21) {
        REQUIRE(std::isfinite(z.real()));
        REQUIRE(std::isfinite(z.imag()));
    }
}

TEST_CASE("dut apply/readback roundtrip", "[simulators]")
{
    DutSimulator dut;
    dut.connect();
    DutState st{};
    st.channel = 3;
    st.att_code = 17;
    st.phase_code = 42;
    dut.apply(st);
    const auto back = dut.readback();
    REQUIRE(back.channel == st.channel);
    REQUIRE(back.att_code == st.att_code);
    REQUIRE(back.phase_code == st.phase_code);
}

TEST_CASE("stub connect fails without opening transport", "[simulators]")
{
    StubDutController stub;
    REQUIRE_THROWS_AS(stub.connect(), std::runtime_error);
    try {
        stub.connect();
        FAIL("expected exception");
    } catch (const std::runtime_error& ex) {
        REQUIRE_THAT(std::string(ex.what()), ContainsSubstring("протокол"));
    }
}
