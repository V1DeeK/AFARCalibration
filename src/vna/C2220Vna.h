#pragma once

#include "IScpiTransport.h"

#include "afar/IVna.h"

#include <cstdint>
#include <string>
#include <vector>

/// Драйвер двухпортовых ПЛАНАР C1220/C2220 / S2VNA поверх IScpiTransport.
/// Только SCPI из docs/contracts/vna-c2220-scpi.md.
class C2220Vna final : public IVna {
public:
    struct Profile {
        /// Явное подтверждение режима прямого доступа в профиле стенда (HW-VNA-05).
        bool allow_direct_access{false};
        /// Подстрока модели в ответе *IDN? (по умолчанию C2220).
        std::string required_model{"C2220"};
        std::uint32_t connect_timeout_ms{3000};
        std::uint32_t sweep_timeout_ms{30000};
        int measure_retries{2};
    };

    explicit C2220Vna(IScpiTransport& transport);
    C2220Vna(IScpiTransport& transport, Profile profile);

    void connect() override;
    std::string identify() override;
    void configure(const SweepConfig& config) override;
    ComplexSweep measure_trace() override;
    ComplexSweep measure_s21() override;
    void calibrate_two_port(TwoPortCalibrationStep step) override;
    std::vector<std::string> drain_errors() override;
    void abort() noexcept override;

    bool connected() const noexcept { return connected_; }
    const Profile& profile() const noexcept { return profile_; }

private:
    void require_connected(const char* op) const;
    void write_cmd(const std::string& cmd);
    std::string query(const std::string& cmd);
    void check_direct_access();
    void reject_if_foreign_model(const std::string& idn) const;
    ComplexSweep measure_once();

    IScpiTransport& transport_;
    Profile profile_;
    bool connected_{false};
    SweepConfig config_{};
    bool configured_{false};
};
