#pragma once

#include "afar/SweepTypes.h"

#include <string>
#include <vector>

class IVna {
public:
    virtual ~IVna() = default;
    virtual void connect() = 0;
    virtual std::string identify() = 0;
    virtual void configure(const SweepConfig&) = 0;
    virtual ComplexSweep measure_s21() = 0;
    virtual std::vector<std::string> drain_errors() = 0;
    virtual void abort() noexcept = 0;
};
