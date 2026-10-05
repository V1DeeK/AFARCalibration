#pragma once

#include "afar/SweepTypes.h"

#include <filesystem>
#include <string>
#include <vector>

namespace afar::report {

struct TwoPortMeasurement {
    SweepConfig requested{};
    SweepConfig applied{};
    bool applied_readback{false};
    ComplexSweep sweep{};
    std::string vna_idn;
    std::string measured_utc;
    double reference_ohm{50.0};
    /// Источник — S2VNA Demo; данные пригодны для демонстрации, не для метрологии.
    bool demo_mode{false};
    /// Пользовательские маркеры графика, привязанные к ближайшей точке свипа.
    std::vector<std::uint64_t> marker_frequency_hz;
    std::string device_name;
    std::string device_serial;
    std::string operator_name;
    std::string comment;
    bool operator_accepted{false};
};

/// Полный согласованный набор S11/S21/S12/S22 на одной частотной оси.
bool validateTwoPortMeasurement(const TwoPortMeasurement& measurement,
                                std::string& diagnostics);

/// Touchstone 1.0: Hz, S, RI, порядок S11 S21 S12 S22.
bool writeTouchstoneS2p(const std::filesystem::path& path,
                        const TwoPortMeasurement& measurement,
                        std::string& diagnostics);

/// PDF A4: титульный лист, четыре полных графических страницы и итог.
bool writeTwoPortReportPdf(const std::filesystem::path& path,
                           const TwoPortMeasurement& measurement,
                           std::string& diagnostics);

}  // namespace afar::report
