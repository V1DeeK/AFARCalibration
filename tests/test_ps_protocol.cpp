#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "PsProtocol.h"

#include <string>
#include <vector>

namespace ps = afar::ps;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

std::vector<ps::ChannelCodes> allChannels()
{
    std::vector<ps::ChannelCodes> ch;
    for (int n = 16; n >= 1; --n) {
        ch.push_back({n, n * 3 % 64, 63 - n});
    }
    return ch;
}

}  // namespace

TEST_CASE("PS: строка NDJSON туда и обратно", "[ps][protocol]")
{
    const auto cmd = ps::makeCommand(ps::msg::SetMode, 53, json{{"mode", "SERVICE"}});
    const std::string line = ps::encodeLine(cmd);
    REQUIRE(line.back() == '\n');
    REQUIRE(line.find('\n') == line.size() - 1);

    ps::Envelope back;
    REQUIRE(ps::decodeLine(line, back));
    CHECK(back.msg_type == "set_mode");
    CHECK(back.msg_class == ps::MsgClass::Command);
    CHECK(back.msg_count == 53);
    CHECK(back.payload.at("mode") == "SERVICE");
}

TEST_CASE("PS: пример ответа get_state из протокола", "[ps][protocol]")
{
    const std::string line =
        R"({"msg_type":"get_state","msg_class":"response","msg_count":3,"payload":{"status":"OK","mode":"OPERATIONAL","state":"OPERATIONAL_IDLE"}})";
    ps::Envelope env;
    REQUIRE(ps::decodeLine(line, env));
    ps::Status st{};
    REQUIRE(ps::responseStatus(env, st));
    CHECK(st == ps::Status::Ok);
    ps::StateInfo info;
    REQUIRE(ps::StateInfo::fromJson(env.payload, info));
    CHECK(info.mode == ps::Mode::Operational);
    CHECK(info.state == "OPERATIONAL_IDLE");
}

TEST_CASE("PS: битые строки отклоняются с понятной причиной", "[ps][protocol]")
{
    ps::Envelope env;
    std::string error;
    CHECK_FALSE(ps::decodeLine("не json", env, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(ps::decodeLine(R"({"msg_class":"command","msg_count":1,"payload":{}})", env, &error));
    CHECK_THAT(error, ContainsSubstring("msg_type"));
    CHECK_FALSE(ps::decodeLine(R"({"msg_type":"x","msg_class":"query","msg_count":1,"payload":{}})", env,
                               &error));
    CHECK_FALSE(ps::decodeLine(R"({"msg_type":"x","msg_class":"command","msg_count":256,"payload":{}})",
                               env, &error));
    CHECK_FALSE(ps::decodeLine(R"({"msg_type":"x","msg_class":"command","msg_count":1.5,"payload":{}})",
                               env, &error));
    CHECK_FALSE(ps::decodeLine(R"({"msg_type":"x","msg_class":"command","msg_count":1,"payload":[]})", env,
                               &error));
}

TEST_CASE("PS: разбивка потока на строки и предел длины", "[ps][protocol]")
{
    ps::LineSplitter s;
    REQUIRE(s.append("{\"a\":1}\n{\"b\""));
    std::string line;
    REQUIRE(s.takeLine(line));
    CHECK(line == "{\"a\":1}");
    CHECK_FALSE(s.takeLine(line));
    REQUIRE(s.append(":2}\r\n"));
    REQUIRE(s.takeLine(line));
    CHECK(line == "{\"b\":2}");

    ps::LineSplitter big;
    const std::string chunk(ps::kMaxLineBytes / 2 + 1, 'x');
    REQUIRE(big.append(chunk));
    CHECK_FALSE(big.append(chunk));
}

TEST_CASE("PS: коды 16 каналов", "[ps][protocol]")
{
    json payload;
    REQUIRE(ps::channelsToJson(allChannels(), payload));
    REQUIRE(payload.at("ch").size() == 16);
    CHECK(payload["ch"][0].at("n") == 1);

    std::vector<ps::ChannelCodes> back;
    REQUIRE(ps::channelsFromJson(payload, back));
    REQUIRE(back.size() == 16);
    CHECK(back[15].n == 16);
    CHECK(back[15].att == 63 - 16);

    std::string error;
    auto ch = allChannels();
    ch.pop_back();
    CHECK_FALSE(ps::channelsToJson(ch, payload, &error));
    CHECK_THAT(error, ContainsSubstring("16"));

    ch = allChannels();
    ch[0].n = ch[1].n;
    CHECK_FALSE(ps::channelsToJson(ch, payload, &error));
    CHECK_THAT(error, ContainsSubstring("повторяется"));

    ch = allChannels();
    ch[3].ph = 64;
    CHECK_FALSE(ps::channelsToJson(ch, payload, &error));

    CHECK_FALSE(ps::channelsFromJson(json{{"ch", json::array({json{{"n", "1"}}})}}, back, &error));
}

TEST_CASE("PS: параметры АЦП", "[ps][protocol]")
{
    const json in = json::parse(R"({"status":"OK","lo":5799998976,"bw":20000000,"fs":30720000,"gain":40})");
    ps::AdcParams adc;
    REQUIRE(ps::AdcParams::fromJson(in, adc));
    CHECK(adc.lo == 5'799'998'976LL);
    CHECK(adc.gain == 40.0);
    CHECK(adc.toJson().at("fs") == 30'720'000);

    adc.lo = 0;
    CHECK_FALSE(adc.validate());
    adc.lo = 200'000'000'000LL;
    CHECK_FALSE(adc.validate());
}

TEST_CASE("PS: состояние луча", "[ps][protocol]")
{
    ps::BeamState b;
    REQUIRE(ps::BeamState::fromJson(json{{"src", "azimuth"}, {"az", 12.5}}, b));
    CHECK(b.src == ps::BeamSource::Azimuth);
    CHECK(b.az == 12.5);
    REQUIRE(ps::BeamState::fromJson(json{{"src", "table"}, {"beam", 7}}, b));
    CHECK(b.beam == 7);
    CHECK_FALSE(ps::BeamState::fromJson(json{{"src", "AZIMUTH"}}, b));
}

TEST_CASE("PS: HEX-строка rs485_send", "[ps][protocol]")
{
    CHECK(ps::hexBytesValid("01A3ff"));
    CHECK_FALSE(ps::hexBytesValid(""));
    CHECK_FALSE(ps::hexBytesValid("01A"));
    CHECK_FALSE(ps::hexBytesValid("0G"));
}

TEST_CASE("PS: телеметрия по предложению контракта", "[ps][telemetry]")
{
    const std::string dg =
        R"({"msg_type":"telemetry","msg_class":"event","msg_count":201,"payload":{"time":1791484800000,"uptime":3605,"mode":"SERVICE","state":"","devices":{"power":"OK","rs485":"FAIL"},"temperature":{"pl":54.5},"power":{"v12":12.05}}})";
    ps::Envelope env;
    REQUIRE(ps::decodeLine(dg, env));
    ps::Telemetry t;
    REQUIRE(ps::Telemetry::fromJson(env.payload, t));
    CHECK(t.mode == ps::Mode::Service);
    REQUIRE(t.devices.size() == 2);
    CHECK(t.temperature.at(0).second == 54.5);
    CHECK_FALSE(ps::Telemetry::fromJson(json{{"time", 1}}, t));
}

TEST_CASE("PS: совместимость версий", "[ps][protocol]")
{
    CHECK(ps::versionsCompatible("1.0", "1.3"));
    CHECK_FALSE(ps::versionsCompatible("2.0", "1.0"));
    CHECK_FALSE(ps::versionsCompatible("", "1.0"));
}

#include "PsServiceCheck.h"

TEST_CASE("Приёмка сервиса: шаги по контракту и оценка ответа", "[ps][check]")
{
    using namespace afar::ps;
    const auto steps = serviceCheckSteps();
    REQUIRE(steps.size() > 20);
    // Начинает с перевода в IDLE, заканчивает в IDLE; последняя запись кодов — 63/0.
    CHECK(steps.front().msgType == msg::GetState);
    CHECK(steps.back().kind == CheckKind::TelemetryClean);
    int udp = 0;
    for (const auto& s : steps) {
        CHECK((s.msgType.rfind("rs485_s", 0) != 0 || s.msgType == msg::Rs485Send));
        if (s.kind == CheckKind::TelemetryMode) {
            ++udp;
        }
    }
    CHECK(udp == 3);
    const CheckStep* lastCodes = nullptr;
    for (const auto& s : steps) {
        CHECK_FALSE(s.title.empty());
        if (s.msgType == msg::SetPhaseAtten && s.expectStatus == Status::Ok) {
            lastCodes = &s;
        }
    }
    REQUIRE(lastCodes);
    REQUIRE(lastCodes->expectCodes.size() == kChannels);
    for (const auto& c : lastCodes->expectCodes) {
        CHECK(c.att == kCodeMax);
        CHECK(c.ph == 0);
    }

    CheckStep beam;
    beam.msgType = std::string(msg::GetBeam);
    beam.expectFields = nlohmann::json{{"src", "azimuth"}, {"az", 12.5}};
    CHECK(evaluate(beam, Status::Ok, {{"src", "azimuth"}, {"az", 12.5}}).verdict == CheckVerdict::Pass);
    CHECK(evaluate(beam, Status::Ok, {{"src", "azimuth"}, {"az", 12.4}}).verdict == CheckVerdict::Fail);
    CHECK(evaluate(beam, Status::Ok, {{"src", "azimuth"}}).detail == "в ответе нет поля az");
    CHECK(evaluate(beam, Status::Busy, {}).verdict == CheckVerdict::Fail);

    CheckStep codes = *lastCodes;
    nlohmann::json payload;
    REQUIRE(channelsToJson(codes.expectCodes, payload));
    CHECK(evaluate(codes, Status::Ok, payload).verdict == CheckVerdict::Pass);
    payload["ch"][4]["att"] = 62;
    const auto bad = evaluate(codes, Status::Ok, payload);
    CHECK(bad.verdict == CheckVerdict::Fail);
    CHECK(bad.detail.find("канал 5") != std::string::npos);
}
