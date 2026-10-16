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
    CHECK(payload["ch"][0].at("en") == 1);

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

    ch = allChannels();
    ch[3].en = 2;
    CHECK_FALSE(ps::channelsToJson(ch, payload, &error));

    CHECK_FALSE(ps::channelsFromJson(json{{"ch", json::array({json{{"n", "1"}}})}}, back, &error));
}

TEST_CASE("PS: состояние луча", "[ps][protocol]")
{
    ps::BeamState b;
    REQUIRE(ps::BeamState::fromJson(json{{"src", "AZIMUTH"}, {"az", 12.5}}, b));
    CHECK(b.src == ps::BeamSource::Azimuth);
    CHECK(b.az == 12.5);
    REQUIRE(ps::BeamState::fromJson(json{{"src", "TABLE"}, {"beam", 7}}, b));
    CHECK(b.beam == 7);
    CHECK_FALSE(ps::BeamState::fromJson(json{{"src", "azimuth"}}, b));
}

TEST_CASE("PS: HEX-строка rs485_send", "[ps][protocol]")
{
    CHECK(ps::hexBytesValid("01A3ff"));
    CHECK_FALSE(ps::hexBytesValid(""));
    CHECK_FALSE(ps::hexBytesValid("01A"));
    CHECK_FALSE(ps::hexBytesValid("0G"));
}

TEST_CASE("PS v8: параметры полного сканирования без period", "[ps][protocol]")
{
    ps::SffParams p;
    REQUIRE(p.validate());
    CHECK_FALSE(p.toJson().contains("period"));
    p.freq_min = ps::kSffFrequencyMinHz - 1;
    CHECK_FALSE(p.validate());
    p = {};
    p.freq_max = ps::kSffFrequencyMaxHz + 1;
    CHECK_FALSE(p.validate());
    p = {};
    ps::SffParams parsed;
    REQUIRE(ps::SffParams::fromJson(p.toJson(), parsed));
    CHECK(parsed.freq_min == p.freq_min);
}

TEST_CASE("PS: команды модулей по v3 §2.5.2", "[ps][protocol][modules]")
{
    using namespace afar::ps;
    nlohmann::json p;
    std::string error;

    // Частичная запись: уходят только заданные поля, частота — целым.
    REQUIRE(loSetPayload(kLoMixer, {{"freq", 5'500'000'000.0}, {"att1", 0.5}, {"pwr_b", 5}}, p, &error));
    CHECK(p == nlohmann::json::parse(R"({"addr":1,"freq":5500000000,"att1":0.5,"pwr_b":5})"));
    CHECK(p["freq"].is_number_integer());
    CHECK(loGetPayload(kLoCalibration) == nlohmann::json{{"addr", 2}});

    CHECK_FALSE(loSetPayload(3, {{"en_rf", 1}}, p, &error));
    CHECK_FALSE(loSetPayload(kLoMixer, {}, p, &error));
    CHECK_FALSE(loSetPayload(kLoMixer, {{"att1", 0.3}}, p, &error));
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("шаг 0,5"));
    CHECK_FALSE(loSetPayload(kLoMixer, {{"att2", 32.0}}, p, &error));
    CHECK_FALSE(loSetPayload(kLoMixer, {{"freq", 7'000'000'001.0}}, p, &error));
    CHECK(loSetPayload(kLoMixer, {{"freq", 7'000'000'000.0}}, p, &error));
    CHECK_FALSE(loSetPayload(kLoMixer, {{"pwr_a", 3}}, p, &error));
    CHECK_FALSE(loSetPayload(kLoMixer, {{"en_b", 2}}, p, &error));
    // У гетеродина нет полей видеоприёмника.
    CHECK_FALSE(loSetPayload(kLoMixer, {{"en_vid", 1}}, p, &error));

    REQUIRE(videoSetPayload({{"en_vid", 1}, {"att_mode", 1}, {"freq", 5'800'000'000.0}, {"att1", 6.0}}, p));
    CHECK(p == nlohmann::json::parse(R"({"en_vid":1,"att_mode":1,"freq":5800000000,"att1":6.0})"));
    CHECK_FALSE(p.contains("addr"));

    REQUIRE(mixerSetPayload({{2, {{"en", 0}}}, {1, {{"en", 1}, {"rp", 0}, {"if_att", 2.5}, {"att", 10.0}}}}, p));
    CHECK(p == nlohmann::json::parse(
                   R"({"ch":[{"n":1,"en":1,"rp":0,"if_att":2.5,"att":10.0},{"n":2,"en":0}]})"));
    CHECK_FALSE(mixerSetPayload({}, p, &error));
    CHECK_FALSE(mixerSetPayload({{5, {{"en", 1}}}}, p, &error));
    CHECK_FALSE(mixerSetPayload({{1, {{"en", 1}}}, {1, {{"rp", 1}}}}, p, &error));
    CHECK_FALSE(mixerSetPayload({{1, {{"if_att", 16.0}}}}, p, &error));
    CHECK_FALSE(mixerSetPayload({{1, {}}}, p, &error));
}

TEST_CASE("PS: ответы модулей — примеры из v3", "[ps][protocol][modules]")
{
    using namespace afar::ps;
    ModuleState lo;
    REQUIRE(ModuleState::fromJson(nlohmann::json::parse(R"({"status":"OK","addr":1,"pwr_b":5,"pwr_a":5,
        "en_b":1,"en_a":1,"en_rf":1,"en_syn":1,"ref":0,"freq":5500000000,"att1":0.5,"att2":31.5,
        "temp":41.5,"lock":1})"),
                                  loFields(), lo));
    CHECK(lo.addr == 1);
    CHECK(lo.values.size() == loFields().size());
    CHECK(lo.values["freq"] == 5'500'000'000.0);
    CHECK(lo.values["att2"] == 31.5);
    REQUIRE(lo.temp);
    CHECK(*lo.temp == 41.5);
    REQUIRE(lo.lock);
    CHECK(*lo.lock == 1);

    std::string error;
    CHECK_FALSE(ModuleState::fromJson(nlohmann::json::parse(R"({"addr":1,"att1":0.3})"), loFields(), lo, &error));

    ModuleState video;
    REQUIRE(ModuleState::fromJson(nlohmann::json::parse(R"({"status":"OK","en_vid":1,"att_mode":1,
        "freq":5800000000,"att1":6.0})"),
                                  videoFields(), video));
    CHECK(video.values.size() == 4);
    CHECK_FALSE(video.temp);

    MixerState mix;
    REQUIRE(MixerState::fromJson(nlohmann::json::parse(R"({"status":"OK","ch":[
        {"n":2,"en":0,"rp":0,"if_att":0.0,"att":0.0},{"n":1,"en":1,"rp":0,"if_att":2.5,"att":10.0}],
        "temp":39.0})"),
                                 mix));
    REQUIRE(mix.ch.size() == 2);
    CHECK(mix.ch[0].n == 1);
    CHECK(mix.ch[0].values["if_att"] == 2.5);
    REQUIRE(mix.temp);
    CHECK_FALSE(MixerState::fromJson(nlohmann::json::parse(R"({"ch":[{"n":5,"en":1}]})"), mix));
    CHECK_FALSE(MixerState::fromJson(nlohmann::json::parse(R"({"ch":[]})"), mix));
}

TEST_CASE("PS v8: 10 байт технического состояния, младший бит первым", "[ps][telemetry]")
{
    ps::Telemetry t;
    std::string packet(10, char(0));
    packet[0] = char(0x01);  // бит 0 — VCCINT исправен, бит 1 — VCCAUX неисправен
    packet[9] = char(0x80);  // бит 79 — калибровочные таблицы исправны
    REQUIRE(ps::Telemetry::fromStatusBytes(packet, t));
    CHECK(t.statusBytes[0] == 1);
    CHECK(t.statusBytes[9] == 128);
    CHECK(t.devices.front().second == ps::DeviceHealth::Ok);
    CHECK(t.devices[1].second == ps::DeviceHealth::Fail);
    CHECK(t.devices.back().second == ps::DeviceHealth::Ok);
    CHECK_FALSE(t.hasMode);
    CHECK_FALSE(ps::Telemetry::fromStatusBytes(packet.substr(0, 9), t));
}

TEST_CASE("PS v8: set_osd и get_osd по таблице 13", "[ps][protocol][osd]")
{
    ps::OsdParams p;
    std::string error;
    CHECK_FALSE(p.validate(&error));

    p.en = 1;
    p.standard = "PAL";
    p.src = "AUTO";
    p.fields = std::vector<std::string>{"state", "freq", "az", "cls", "snr"};
    p.hos = 0;
    p.vos = 0;
    REQUIRE(p.validate(&error));
    CHECK(p.toJson() == json::parse(R"({"en":1,"std":"PAL","src":"AUTO",
        "fields":["state","freq","az","cls","snr"],"hos":0,"vos":0})"));

    ps::OsdParams only;
    only.src = "TEXT";
    CHECK(only.toJson() == json{{"src", "TEXT"}});

    ps::OsdParams bad = p;
    bad.hos = 32;
    CHECK_FALSE(bad.validate(&error));
    bad = p;
    bad.vos = -33;
    CHECK_FALSE(bad.validate(&error));
    bad = p;
    bad.standard = "SECAM";
    CHECK_FALSE(bad.validate(&error));
    bad = p;
    bad.fields = std::vector<std::string>{"state", "state"};
    CHECK_FALSE(bad.validate(&error));
    bad = p;
    bad.fields = std::vector<std::string>{"rssi"};
    CHECK_FALSE(bad.validate(&error));

    // Пример ответа из v8 п. 3.3.1.1.
    const json reply = json::parse(R"({"status":"OK","en":1,"std":"PAL","src":"AUTO",
        "fields":["state","freq","az","cls","snr"],"hos":0,"vos":0,"los":0})");
    ps::OsdParams got;
    REQUIRE(ps::OsdParams::fromJson(reply, got, &error));
    CHECK(got.los == 0);
    CHECK(got.fields->size() == 5);
    CHECK_FALSE(ps::OsdParams::fromJson(json{{"en", 1}, {"los", 2}}, got));
    CHECK_FALSE(ps::OsdParams::fromJson(json{{"fields", "state"}}, got));
}

TEST_CASE("PS v8: set_osd_text по таблице 34", "[ps][protocol][osd]")
{
    json payload;
    std::string error;
    REQUIRE(ps::osdTextPayload(2, 1, "TEST LO 5800 MHZ", false, payload, &error));
    CHECK(payload == json{{"row", 2}, {"col", 1}, {"text", "TEST LO 5800 MHZ"}});

    CHECK(ps::osdTextPayload(15, 0, "A", false, payload));
    CHECK_FALSE(ps::osdTextPayload(15, 0, "A", true, payload, &error));
    CHECK_THAT(error, ContainsSubstring("NTSC"));
    CHECK(ps::osdTextPayload(12, 0, "A", true, payload));
    CHECK_FALSE(ps::osdTextPayload(-1, 0, "A", false, payload));
    CHECK_FALSE(ps::osdTextPayload(0, 30, "A", false, payload));
    CHECK(ps::osdTextPayload(0, 29, "A", false, payload));
    CHECK_FALSE(ps::osdTextPayload(0, 29, "AB", false, payload));
    CHECK(ps::osdTextPayload(0, 0, std::string(30, 'X'), false, payload));
    CHECK_FALSE(ps::osdTextPayload(0, 0, std::string(31, 'X'), false, payload));
    CHECK_FALSE(ps::osdTextPayload(0, 0, "", false, payload));
    CHECK_FALSE(ps::osdTextPayload(0, 0, "Тест", false, payload));
    CHECK_FALSE(ps::osdTextPayload(0, 0, "A\tB", false, payload));
    CHECK(ps::osdTextPayload(0, 0, " !~", false, payload));
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
