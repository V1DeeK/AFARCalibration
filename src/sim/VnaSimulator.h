#pragma once

#include "afar/IVna.h"

#include <stdexcept>
#include <string>
#include <vector>

/// Программный имитатор VNA (C2220). Не знает att/phase изделия —
/// синтетический S21 зависит только от частоты после configure.
class VnaSimulator final : public IVna {
public:
    enum class FailureMode {
        None,
        Timeout,
        Disconnect,
        Overload,
        Nan,
        WrongModel,
    };

    void connect() override;
    std::string identify() override;
    void configure(const SweepConfig& config) override;
    ComplexSweep measure_trace() override;
    ComplexSweep measure_s21() override;
    std::vector<std::string> drain_errors() override;
    void abort() noexcept override;

    void set_failure_mode(FailureMode mode);
    void set_identify_string(std::string idn);
    void push_instrument_error(std::string message);

    /// Прямой доступ к приёмникам (SYSTem:RECeiver:DIRect:ACCess).
    /// Запрос состояния — позже у оркестратора; здесь только флаг.
    bool direct_access_on() const noexcept { return directAccessOn_; }
    void set_direct_access_on(bool on) noexcept { directAccessOn_ = on; }

    bool connected() const noexcept { return connected_; }
    const SweepConfig& last_config() const noexcept { return config_; }

private:
    void throw_if_failure_on_io(const char* op);

    bool connected_{false};
    bool configured_{false};
    bool directAccessOn_{false};
    FailureMode failureMode_{FailureMode::None};
    std::string identifyString_{
        "PLANAR,C2220,SIM0001,1.0"};
    SweepConfig config_{};
    std::vector<std::string> errorQueue_;
};
