#pragma once

#include "afar/IDutController.h"

#include <stdexcept>

/// Программный имитатор контроллера АФАР этапа 1.
class DutSimulator final : public IDutController {
public:
    void connect() override;
    void apply(const DutState& state) override;
    DutState readback() override;
    double temperature_c() override;
    void set_safe_state() noexcept override;

    /// Следующий apply() откажет подтверждение (AT-08 заготовка).
    void reject_next_apply();

    /// Имитация обрыва связи: последующие I/O бросают, кроме set_safe_state.
    void simulate_disconnect();

    void set_temperature_c(double t_c) noexcept { temperatureC_ = t_c; }
    bool connected() const noexcept { return connected_; }

private:
    void ensure_connected(const char* op) const;

    bool connected_{false};
    bool rejectNextApply_{false};
    double temperatureC_{25.0};
    DutState state_{};
};

