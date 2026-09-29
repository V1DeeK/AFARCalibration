#pragma once

#include "afar/SweepTypes.h"

#include <filesystem>
#include <string>

namespace afar::report {

struct TwoPortMeasurement {
    SweepConfig requested{};
    SweepConfig applied{};
    bool applied_readback{false};
    ComplexSweep sweep{};
    std::string vna_idn;
    std::string measured_utc;
    double reference_ohm{50.0};
};

/// Полный согласованный набор S11/S21/S12/S22 на одной частотной оси.
bool validateTwoPortMeasurement(const TwoPortMeasurement& measurement,
                                std::string& diagnostics);

/// Touchstone 1.0: Hz, S, RI, порядок S11 S21 S12 S22.
bool writeTouchstoneS2p(const std::filesystem::path& path,
                        const TwoPortMeasurement& measurement,
                        std::string& diagnostics);

/// PDF A4: метаданные, сводка и графики модуля четырёх S-параметров.
bool writeTwoPortReportPdf(const std::filesystem::path& path,
                           const TwoPortMeasurement& measurement,
                           std::string& diagnostics);

}  // namespace afar::report
