#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "DutSimulator.h"
#include "StubDutController.h"
#include "VnaSimulator.h"

#include <cmath>
#include <string>

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("AT-01 identify contains C2220", "[simulators]")
{
    VnaSimulator vna;
    vna.connect();
    const auto idn = vna.identify();
    REQUIRE_THAT(idn, ContainsSubstring("C2220"));
    REQUIRE_THAT(idn, ContainsSubstring("PLANAR"));
    // Типичный CSV *IDN?: manufacturer,model,serial,firmware
    REQUIRE(idn == "PLANAR,C2220,SIM0001,1.0");
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
    cfg.s_parameter = SParameter::S21;
    vna.configure(cfg);

    const auto sweep = vna.measure_s21();
    REQUIRE(sweep.frequency_hz.size() == cfg.points);
    REQUIRE(sweep.s21.size() == cfg.points);
    REQUIRE(sweep.s11.empty());
    REQUIRE(sweep.s12.empty());
    REQUIRE(sweep.s22.empty());
    REQUIRE_FALSE(sweep.overload);
    REQUIRE(sweep.frequency_hz.front() == cfg.f_start_hz);
    REQUIRE(sweep.frequency_hz.back() == cfg.f_stop_hz);
    for (const auto& z : sweep.s21) {
        REQUIRE(std::isfinite(z.real()));
        REQUIRE(std::isfinite(z.imag()));
        REQUIRE(std::abs(z) == Approx(0.8).margin(1e-9));
    }
}

TEST_CASE("measure_trace S11 fills s11 and measure_s21 rejects", "[simulators]")
{
    VnaSimulator vna;
    vna.connect();
    SweepConfig cfg{};
    cfg.f_start_hz = 1'000'000'000ULL;
    cfg.f_stop_hz = 2'000'000'000ULL;
    cfg.points = 3;
    cfg.power_dbm = -10.0;
    cfg.ifbw_hz = 1000;
    cfg.averages = 1;
    cfg.s_parameter = SParameter::S11;
    vna.configure(cfg);

    const auto sweep = vna.measure_trace();
    REQUIRE(sweep.s11.size() == cfg.points);
    REQUIRE(sweep.s21.empty());
    REQUIRE(sweep.s12.empty());
    REQUIRE(sweep.s22.empty());
    for (const auto& z : sweep.s11) {
        REQUIRE(std::abs(z) == Approx(0.3).margin(1e-9));
    }

    REQUIRE_THROWS_AS(vna.measure_s21(), std::runtime_error);
}

TEST_CASE("VnaSimulator calibrate_one_port stores step and port", "[simulators]")
{
    VnaSimulator vna;
    REQUIRE_THROWS_AS(vna.calibrate_one_port(OnePortCalibrationStep::Begin, 1),
                      std::runtime_error);
    vna.connect();
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Begin, 2));
    REQUIRE(vna.last_one_port_step() == OnePortCalibrationStep::Begin);
    REQUIRE(vna.last_one_port() == 2);
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Open, 2));
    REQUIRE(vna.last_one_port_step() == OnePortCalibrationStep::Open);
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Short, 2));
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Load, 2));
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Apply, 2));
    REQUIRE(vna.last_one_port_step() == OnePortCalibrationStep::Apply);
    REQUIRE_THROWS_AS(vna.calibrate_one_port(OnePortCalibrationStep::Begin, 0),
                      std::runtime_error);
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

TEST_CASE("linked demo stand changes S21 with channel attenuation and phase", "[simulators][demo]")
{
    DutSimulator dut;
    VnaSimulator vna;
    dut.connect();
    vna.connect();
    vna.set_dut_state_provider([&dut] { return dut.current_state(); });

    SweepConfig cfg{};
    cfg.f_start_hz = 1'200'000'000ULL;
    cfg.f_stop_hz = 1'300'000'000ULL;
    cfg.points = 3;
    cfg.ifbw_hz = 1000;
    cfg.power_dbm = -20.0;
    cfg.averages = 1;
    cfg.s_parameter = SParameter::S21;
    vna.configure(cfg);

    dut.apply({1, 0, 0});
    const auto reference = vna.measure_s21().s21;
    REQUIRE(reference.size() == 3);
    REQUIRE(vna.measure_s21().s21 == reference);

    dut.apply({1, 20, 0});
    const auto attenuated = vna.measure_s21().s21;
    CHECK(std::abs(attenuated[1]) < std::abs(reference[1]));

    dut.apply({1, 20, 16});
    const auto phaseShifted = vna.measure_s21().s21;
    const double phaseDeltaDeg = std::arg(phaseShifted[1] / attenuated[1]) * 180.0
        / 3.14159265358979323846;
    CHECK(phaseDeltaDeg == Approx(90.0).margin(1.0));

    dut.apply({2, 20, 16});
    const auto otherChannel = vna.measure_s21().s21;
    CHECK(otherChannel[1] != phaseShifted[1]);
    CHECK(otherChannel == vna.measure_s21().s21);
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
