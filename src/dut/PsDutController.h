#pragma once

#include "afar/IDutController.h"

#include "PsProtocol.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QTcpSocket;
class QUdpSocket;

/// Контроллер АФАР через PS изделия (docs/contracts/ps-json-protocol.md §4.1, §7).
/// Решётку меняет PS по set_phase_atten; программа — только клиент.
///
/// Блокирующий драйвер для рабочего потока оркестратора (ТЗ т. 8.5): своё
/// TCP-соединение и приём телеметрии, без событийного цикла. PS держит одно
/// соединение, поэтому на время серии ручная связь вкладки PS закрывается.
class PsDutController final : public IDutController {
public:
    struct Options {
        std::string host;
        std::uint16_t port = afar::ps::kDefaultControlPort;
        /// 0 — любой свободный порт (тесты).
        std::uint16_t telemetry_port = afar::ps::kDefaultTelemetryPort;
        int timeout_ms = 2000;
        /// Коды остальных 15 каналов при измерении одного и безопасное
        /// состояние всех 16. Решение пользователя 2026-10-12: att 63, фаза 0.
        int inactive_att_code = 63;
        int inactive_phase_code = 0;
        /// Ключ температуры в телеметрии; пусто — первая из переданных.
        std::string temperature_key;
    };

    explicit PsDutController(Options options);
    ~PsDutController() override;

    PsDutController(const PsDutController&) = delete;
    PsDutController& operator=(const PsDutController&) = delete;

    /// TCP, hello, перевод PS в SERVICE (из IDLE/READY). Ошибка — исключение.
    void connect() override;
    /// Коды активного канала + inactive_* на остальных; в ответе PS — коды,
    /// считанные из микросхем. Расхождение хотя бы в одном канале — исключение.
    void apply(const DutState& state) override;
    /// Коды активного канала, считанные из микросхем при последней записи;
    /// до первой записи — get_phase_atten.
    DutState readback() override;
    /// Температура из UDP-телеметрии PS.
    double temperature_c() override;
    /// Все 16 каналов в inactive_*; ошибки глотаются (noexcept по т. 8.3).
    void set_safe_state() noexcept override;

    void disconnect() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    /// Коды всех 16 каналов из последнего ответа PS.
    [[nodiscard]] const std::vector<afar::ps::ChannelCodes>& lastReadback() const noexcept
    {
        return readback_;
    }

private:
    afar::ps::Envelope request(std::string_view msgType, const nlohmann::json& payload);
    void requireOk(const afar::ps::Envelope& response);
    std::vector<afar::ps::ChannelCodes> frameFor(const DutState& state) const;
    std::optional<double> drainTelemetry();

    Options options_;
    std::unique_ptr<QTcpSocket> socket_;
    std::unique_ptr<QUdpSocket> udp_;
    afar::ps::LineSplitter splitter_;
    std::uint8_t tx_count_ = 0;
    std::uint8_t active_channel_ = 1;
    std::vector<afar::ps::ChannelCodes> readback_;
    std::optional<double> last_temperature_;
};
