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
    /// Один свип текущего `CALC:PAR:DEF` (после `configure`).
    /// Заполняет `frequency_hz` и ровно один из `s11`/`s21`/`s12`/`s22`
    /// по `SweepConfig::s_parameter`; остальные векторы пусты.
    virtual ComplexSweep measure_trace() = 0;
    /// S21-only: ожидает `s_parameter == S21` после `configure`.
    /// Эквивалент `measure_trace()` при DEF=S21; иначе ошибка.
    virtual ComplexSweep measure_s21() = 0;
    virtual void calibrate_two_port(TwoPortCalibrationStep step) = 0;
    virtual std::vector<std::string> drain_errors() = 0;
    virtual void abort() noexcept = 0;
};
