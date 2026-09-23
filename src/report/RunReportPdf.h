#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace afar::report {

struct RunReportInfo {
    std::string run_id;
    std::size_t completed_states{0};
    std::size_t valid_direct_count{0};
    std::string software_version{"0.1.0"};
    std::string series_path;
};

/// Минимальный валидный PDF 1.4 (текстовый) с run_id, completed, версией ПО.
bool writeRunReportPdf(const std::filesystem::path& path,
                       const RunReportInfo& info,
                       std::string& diagnostics);

/// Смоук: файл начинается с `%PDF-` и ненулевой.
bool isValidPdfSmoke(const std::filesystem::path& path, std::string& diagnostics);

}  // namespace afar::report
