#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Расчёт кодов луча по beam_reference.py (C:\Users\user\Documents\beam_reference.py).
// Две антенны, колонки 1…16: одно решение на частоту и угол, затем общий CSV
// и по файлу на изделие (колонки этого изделия — срез того же решения).

struct BeamTableRequest {
    std::vector<std::string> measurementsCsv;
    double targetGainDb = 29.0;
    double startHz = 4.9e9;
    double stopHz = 6.0e9;
    double stepHz = 1.0e6;
    std::string outputDir;
};

[[nodiscard]] std::string writeBeamCodeTables(const BeamTableRequest& request,
                                              const std::function<void(const std::string&)>& progress);
