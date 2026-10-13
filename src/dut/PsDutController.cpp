#include "PsDutController.h"

#include <QDeadlineTimer>
#include <QHostAddress>
#include <QNetworkDatagram>
#include <QTcpSocket>
#include <QUdpSocket>

#include <stdexcept>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

[[noreturn]] void fail(const std::string& text)
{
    throw std::runtime_error("PS: " + text);
}

}  // namespace

PsDutController::PsDutController(Options options)
    : options_(std::move(options))
{
}

PsDutController::~PsDutController()
{
    // В деструкторе без команд железу (т. 8.3): только закрыть сокеты.
    disconnect();
}

bool PsDutController::connected() const noexcept
{
    return socket_ && socket_->state() == QAbstractSocket::ConnectedState;
}

void PsDutController::disconnect() noexcept
{
    if (socket_) {
        socket_->abort();
        socket_.reset();
    }
    if (udp_) {
        udp_->close();
        udp_.reset();
    }
    splitter_.clear();
    readback_.clear();
}

void PsDutController::connect()
{
    disconnect();
    if (options_.host.empty()) {
        fail("адрес PS не задан");
    }
    udp_ = std::make_unique<QUdpSocket>();
    if (!udp_->bind(QHostAddress::AnyIPv4, options_.telemetry_port,
                    QAbstractSocket::ShareAddress | QAbstractSocket::ReuseAddressHint)) {
        fail("порт телеметрии UDP " + std::to_string(options_.telemetry_port) + " занят");
    }
    socket_ = std::make_unique<QTcpSocket>();
    socket_->connectToHost(QString::fromStdString(options_.host), options_.port);
    if (!socket_->waitForConnected(options_.timeout_ms)) {
        const std::string why = socket_->errorString().toStdString();
        disconnect();
        fail(options_.host + ":" + std::to_string(options_.port) + " не отвечает: " + why);
    }

    QHostAddress local = socket_->localAddress();
    bool isV4 = false;
    const quint32 v4 = local.toIPv4Address(&isV4);
    if (isV4) {
        local = QHostAddress(v4);
    }
    ps::Hello hello;
    hello.tlm_ip = local.toString().toStdString();
    hello.tlm_port = udp_->localPort();
    const auto helloReply = request(ps::msg::Hello, hello.toJson());
    requireOk(helloReply);
    const auto ver = helloReply.payload.find("ver");
    if (ver == helloReply.payload.end() || !ver->is_string() ||
        !ps::versionsCompatible(ver->get<std::string>(), ps::kProtocolVersion)) {
        fail("несовместимая версия протокола PS");
    }

    ps::StateInfo info;
    std::string error;
    const auto state = request(ps::msg::GetState, json::object());
    requireOk(state);
    if (!ps::StateInfo::fromJson(state.payload, info, &error)) {
        fail("get_state: " + error);
    }
    if (info.mode != ps::Mode::Service) {
        if (info.mode != ps::Mode::Idle || info.state != "READY") {
            fail("PS в режиме " + std::string(ps::toString(info.mode)) + "/" + info.state +
                 ", в SERVICE можно перейти только из IDLE/READY");
        }
        requireOk(request(ps::msg::SetMode, json{{"mode", "SERVICE"}}));
    }
}

ps::Envelope PsDutController::request(std::string_view msgType, const json& payload)
{
    if (!connected()) {
        fail("нет связи");
    }
    const std::uint8_t count = ++tx_count_;
    const std::string line = ps::encodeLine(ps::makeCommand(msgType, count, payload));
    socket_->write(line.data(), static_cast<qint64>(line.size()));
    if (!socket_->waitForBytesWritten(options_.timeout_ms)) {
        fail(std::string(msgType) + ": не удалось отправить");
    }

    const QDeadlineTimer deadline(options_.timeout_ms);
    std::string text;
    for (;;) {
        while (splitter_.takeLine(text)) {
            ps::Envelope env;
            // Битые строки и события PS здесь пропускаем: ждём свой ответ.
            if (!ps::decodeLine(text, env) || env.msg_class != ps::MsgClass::Response) {
                continue;
            }
            if (env.msg_type == msgType && env.msg_count == count) {
                return env;
            }
        }
        const int left = static_cast<int>(deadline.remainingTime());
        if (deadline.hasExpired() || !socket_->waitForReadyRead(left)) {
            fail(std::string(msgType) + ": нет ответа за " + std::to_string(options_.timeout_ms) +
                 " мс");
        }
        const QByteArray bytes = socket_->readAll();
        if (!splitter_.append(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())))) {
            disconnect();
            fail("строка ответа длиннее 1 МиБ");
        }
    }
}

void PsDutController::requireOk(const ps::Envelope& response)
{
    ps::Status st{};
    if (!ps::responseStatus(response, st)) {
        fail(response.msg_type + ": ответ без status");
    }
    if (st != ps::Status::Ok) {
        fail(response.msg_type + ": " + std::string(ps::toString(st)) + " (" + ps::statusRu(st) + ")");
    }
}

std::vector<ps::ChannelCodes> PsDutController::frameFor(const DutState& state) const
{
    std::vector<ps::ChannelCodes> ch;
    ch.reserve(ps::kChannels);
    for (int n = 1; n <= ps::kChannels; ++n) {
        if (n == state.channel) {
            ch.push_back({n, state.phase_code, state.att_code});
        } else {
            ch.push_back({n, options_.inactive_phase_code, options_.inactive_att_code});
        }
    }
    return ch;
}

void PsDutController::apply(const DutState& state)
{
    if (state.channel < 1 || state.channel > ps::kChannels || state.att_code > ps::kCodeMax ||
        state.phase_code > ps::kCodeMax) {
        fail("канал " + std::to_string(state.channel) + " или коды вне диапазона");
    }
    const auto expected = frameFor(state);
    json payload;
    std::string error;
    if (!ps::channelsToJson(expected, payload, &error)) {
        fail(error);
    }
    readback_.clear();
    const auto reply = request(ps::msg::SetPhaseAtten, payload);
    requireOk(reply);
    std::vector<ps::ChannelCodes> got;
    if (!ps::channelsFromJson(reply.payload, got, &error)) {
        fail("set_phase_atten: " + error);
    }
    readback_ = got;
    active_channel_ = state.channel;
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (!(got[i] == expected[i])) {
            fail("канал " + std::to_string(got[i].n) + ": записано ph=" +
                 std::to_string(expected[i].ph) + " att=" + std::to_string(expected[i].att) +
                 ", прочитано ph=" + std::to_string(got[i].ph) + " att=" + std::to_string(got[i].att));
        }
    }
}

DutState PsDutController::readback()
{
    if (readback_.empty()) {
        const auto reply = request(ps::msg::GetPhaseAtten, json::object());
        requireOk(reply);
        std::string error;
        if (!ps::channelsFromJson(reply.payload, readback_, &error)) {
            fail("get_phase_atten: " + error);
        }
    }
    const auto& c = readback_.at(static_cast<std::size_t>(active_channel_ - 1));
    DutState out;
    out.channel = static_cast<std::uint8_t>(c.n);
    out.att_code = static_cast<std::uint16_t>(c.att);
    out.phase_code = static_cast<std::uint8_t>(c.ph);
    return out;
}

std::optional<double> PsDutController::drainTelemetry()
{
    std::optional<double> found;
    while (udp_ && udp_->hasPendingDatagrams()) {
        const QNetworkDatagram dg = udp_->receiveDatagram(static_cast<qint64>(ps::kMaxDatagramBytes) + 1);
        const QByteArray data = dg.data();
        ps::Envelope env;
        ps::Telemetry t;
        if (data.size() > static_cast<qsizetype>(ps::kMaxDatagramBytes) ||
            !ps::decodeLine(std::string_view(data.constData(), static_cast<std::size_t>(data.size())), env) ||
            env.msg_type != ps::msg::Telemetry || !ps::Telemetry::fromJson(env.payload, t)) {
            continue;
        }
        for (const auto& [key, value] : t.temperature) {
            if (options_.temperature_key.empty() || key == options_.temperature_key) {
                found = value;
                break;
            }
        }
    }
    return found;
}

double PsDutController::temperature_c()
{
    if (!udp_) {
        fail("нет связи");
    }
    if (auto t = drainTelemetry()) {
        last_temperature_ = t;
        return *t;
    }
    // Телеметрия идёт раз в секунду: ждём одну датаграмму не дольше тайм-аута.
    if (udp_->waitForReadyRead(options_.timeout_ms)) {
        if (auto t = drainTelemetry()) {
            last_temperature_ = t;
            return *t;
        }
    }
    if (last_temperature_) {
        return *last_temperature_;
    }
    fail("в телеметрии нет температуры");
}

void PsDutController::set_safe_state() noexcept
{
    try {
        if (!connected()) {
            return;
        }
        DutState none{};
        none.channel = 0;  // ни один канал не активен
        json payload;
        ps::channelsToJson(frameFor(none), payload);
        readback_.clear();
        const auto reply = request(ps::msg::SetPhaseAtten, payload);
        std::vector<ps::ChannelCodes> got;
        if (ps::channelsFromJson(reply.payload, got)) {
            readback_ = got;
        }
    } catch (...) {
        // Безопасный останов не должен бросать (т. 8.3).
    }
}
