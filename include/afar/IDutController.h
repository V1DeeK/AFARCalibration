#pragma once

#include <cstdint>

struct DutState {
    std::uint8_t channel{};
    std::uint16_t att_code{};
    std::uint8_t phase_code{};
};

class IDutController {
public:
    virtual ~IDutController() = default;
    virtual void connect() = 0;
    virtual void apply(const DutState&) = 0;
    virtual DutState readback() = 0;
    virtual double temperature_c() = 0;
    virtual void set_safe_state() noexcept = 0;
};
