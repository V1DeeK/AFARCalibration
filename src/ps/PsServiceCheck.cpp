#include "PsServiceCheck.h"

#include <cmath>
#include <sstream>

namespace afar::ps {

using nlohmann::json;

namespace {

std::vector<ChannelCodes> allChannels(int ph, int att)
{
    std::vector<ChannelCodes> v;
    for (int n = 1; n <= kChannels; ++n) {
        v.push_back({n, ph, att});
    }
    return v;
}

/// Коды с разными значениями по каналам — так видно перепутанные каналы.
std::vector<ChannelCodes> rampChannels()
{
    std::vector<ChannelCodes> v;
    for (int n = 1; n <= kChannels; ++n) {
        v.push_back({n, (n * 4) % (kCodeMax + 1), (n * 3) % (kCodeMax + 1)});
    }
    return v;
}

json codesPayload(const std::vector<ChannelCodes>& codes)
{
    json p;
    channelsToJson(codes, p);
    return p;
}

class Builder {
public:
    std::string phase;
    std::vector<CheckStep> steps;

    CheckStep& add(std::string title, std::string_view type, Status status, std::string section,
                   json payload = json::object())
    {
        CheckStep s;
        s.phase = phase;
        s.title = std::move(title);
        s.msgType = std::string(type);
        s.payload = std::move(payload);
        s.expectStatus = status;
        s.section = std::move(section);
        steps.push_back(std::move(s));
        return steps.back();
    }

    void state(std::string_view mode, std::string_view st = {})
    {
        json f{{"mode", std::string(mode)}};
        if (!st.empty()) {
            f["state"] = std::string(st);
        }
        add("состояние " + std::string(mode), msg::GetState, Status::Ok, "3").expectFields = f;
    }
};

bool sameValue(const json& want, const json& got)
{
    if (want.is_number() && got.is_number()) {
        const double w = want.get<double>();
        return std::fabs(w - got.get<double>()) <= 1e-6 * std::max(1.0, std::fabs(w));
    }
    return want == got;
}

std::string show(const json& v)
{
    if (v.is_string()) {
        return v.get<std::string>();
    }
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    if (v.is_array()) {
        return "[" + std::to_string(v.size()) + "]";
    }
    if (v.is_object()) {
        return "{…}";
    }
    return v.dump();
}

}  // namespace

std::vector<CheckStep> serviceCheckSteps()
{
    Builder b;
    const AdcParams adc{5'799'936'000, 20'000'000, 30'720'000, 40.0};
    const auto ramp = rampChannels();
    const auto safe = allChannels(0, kCodeMax);

    b.phase = "Связь";
    b.add("PS отвечает", msg::GetState, Status::Ok, "3");
    b.add("перевод в ожидание", msg::SetMode, Status::Ok, "3", json{{"mode", "IDLE"}});
    b.state("IDLE", "READY");
    b.add("коды вне SERVICE — отказ", msg::SetPhaseAtten, Status::NotSupported, "4, 6",
          codesPayload(safe));
    b.add("АЦП вне SERVICE — отказ", msg::GetAdc, Status::NotSupported, "4, 6");

    b.phase = "Вход в SERVICE";
    b.add("неизвестный режим — отказ", msg::SetMode, Status::InvalidArgument, "3",
          json{{"mode", "CALIBRATION"}});
    b.add("переход в сервисный режим", msg::SetMode, Status::Ok, "3", json{{"mode", "SERVICE"}})
        .expectFields = json{{"mode", "SERVICE"}};
    b.state("SERVICE");

    b.phase = "Приёмный тракт";
    b.add("чтение АЦП", msg::GetAdc, Status::Ok, "4.2");
    b.add("настройка АЦП", msg::SetAdc, Status::Ok, "4.2", adc.toJson());
    // lo может сдвинуться на шаг сетки синтезатора (§4.2), его не сравниваем.
    b.add("АЦП прочитан тем же", msg::GetAdc, Status::Ok, "4.2").expectFields =
        json{{"bw", adc.bw}, {"fs", adc.fs}, {"gain", adc.gain}};
    b.add("lo = 0 — отказ", msg::SetAdc, Status::InvalidArgument, "4.2",
          json{{"lo", 0}, {"bw", adc.bw}, {"fs", adc.fs}, {"gain", adc.gain}});

    b.phase = "Луч";
    b.add("азимут луча", msg::SetBeamAzimuth, Status::Ok, "4.3", json{{"az", 12.5}});
    b.add("луч задан азимутом", msg::GetBeam, Status::Ok, "4.3").expectFields =
        json{{"src", "azimuth"}, {"az", 12.5}};
    b.add("азимут вне ±30° — отказ", msg::SetBeamAzimuth, Status::InvalidArgument, "4.3",
          json{{"az", 45.0}});
    b.add("луч из таблицы", msg::SetBeam, Status::Ok, "4.3", json{{"beam", 7}});
    b.add("луч задан таблицей", msg::GetBeam, Status::Ok, "4.3").expectFields =
        json{{"src", "table"}, {"beam", 7}};

    b.phase = "Коды решётки";
    b.add("чтение кодов из микросхем", msg::GetPhaseAtten, Status::Ok, "4.1");
    b.add("запись разных кодов по каналам", msg::SetPhaseAtten, Status::Ok, "4.1",
          codesPayload(ramp))
        .expectCodes = ramp;
    b.add("коды прочитаны те же", msg::GetPhaseAtten, Status::Ok, "4.1").expectCodes = ramp;
    b.add("луч задан кодами", msg::GetBeam, Status::Ok, "4.3").expectFields = json{{"src", "codes"}};
    {
        json bad = codesPayload(safe);
        bad["ch"].erase(bad["ch"].size() - 1);
        b.add("15 каналов вместо 16 — отказ", msg::SetPhaseAtten, Status::InvalidArgument, "4.1",
              bad);
    }
    b.add("коды не изменились после отказа", msg::GetPhaseAtten, Status::Ok, "4.1").expectCodes =
        ramp;
    b.add("безопасное состояние 63/0", msg::SetPhaseAtten, Status::Ok, "4.1, 7", codesPayload(safe))
        .expectCodes = safe;

    b.phase = "RS-485";
    b.add("байты в линию", msg::Rs485Send, Status::Ok, "4.4", json{{"data", "01A3FF"}});
    b.add("нечётная HEX-строка — отказ", msg::Rs485Send, Status::InvalidArgument, "4.4",
          json{{"data", "ABC"}});
    b.add("статус гетеродина смесителя", msg::Rs485Status, Status::Ok, "4.5",
          rs485StatusPayload(Rs485Module::MixerLo))
        .proposal = true;
    b.steps.back().expectFields = json{{"addr", 1}};
    b.add("неизвестный адрес модуля — отказ", msg::Rs485Status, Status::InvalidArgument, "4.5",
          json{{"addr", 9}})
        .proposal = true;

    b.phase = "Завершение";
    b.add("возврат в ожидание", msg::SetMode, Status::Ok, "3", json{{"mode", "IDLE"}});
    b.state("IDLE", "READY");
    b.add("сервисные команды снова отклоняются", msg::GetBeam, Status::NotSupported, "6");
    return b.steps;
}

CheckResult evaluate(const CheckStep& step, Status status, const json& payload)
{
    if (status != step.expectStatus) {
        return {CheckVerdict::Fail, "статус " + std::string(toString(status)) + ", ожидали " +
                                        std::string(toString(step.expectStatus))};
    }
    for (const auto& [key, want] : step.expectFields.items()) {
        const auto it = payload.find(key);
        if (it == payload.end()) {
            return {CheckVerdict::Fail, "в ответе нет поля " + key};
        }
        if (!sameValue(want, *it)) {
            return {CheckVerdict::Fail, key + " = " + show(*it) + ", ожидали " + show(want)};
        }
    }
    if (!step.expectCodes.empty()) {
        std::vector<ChannelCodes> got;
        std::string error;
        if (!channelsFromJson(payload, got, &error)) {
            return {CheckVerdict::Fail, "коды не разобраны: " + error};
        }
        for (std::size_t i = 0; i < got.size(); ++i) {
            const auto& g = got[i];
            const auto& w = step.expectCodes[i];
            if (g != w) {
                return {CheckVerdict::Fail, "канал " + std::to_string(g.n) + ": ph " +
                                                std::to_string(g.ph) + ", att " +
                                                std::to_string(g.att) + ", ожидали " +
                                                std::to_string(w.ph) + "/" + std::to_string(w.att)};
            }
        }
    }
    return {CheckVerdict::Pass, {}};
}

std::string verdictRu(CheckVerdict v)
{
    switch (v) {
    case CheckVerdict::NotRun: return "не выполнялся";
    case CheckVerdict::Pass: return "пройден";
    case CheckVerdict::Fail: return "не пройден";
    case CheckVerdict::NoAnswer: return "нет ответа";
    }
    return {};
}

std::string expectedText(const CheckStep& step)
{
    std::string s(toString(step.expectStatus));
    for (const auto& [key, want] : step.expectFields.items()) {
        s += ", " + key + "=" + show(want);
    }
    if (!step.expectCodes.empty()) {
        s += ", коды 16 каналов как заданы";
    }
    return s;
}

std::string responseText(Status status, const json& payload)
{
    std::string s(toString(status));
    if (!payload.is_object()) {
        return s;
    }
    for (const auto& [key, value] : payload.items()) {
        if (key == "status") {
            continue;
        }
        s += ", " + key + "=" + show(value);
    }
    return s;
}

}  // namespace afar::ps
