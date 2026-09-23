#pragma once

#include "afar/IDutController.h"

/// Заглушка драйвера реального контроллера АФАР.
/// Не открывает COM/TCP: протокол изделия не передан (т. 14 ТЗ).
class StubDutController final : public IDutController {
public:
    void connect() override;
    void apply(const DutState& state) override;
    DutState readback() override;
    double temperature_c() override;
    void set_safe_state() noexcept override;
};
