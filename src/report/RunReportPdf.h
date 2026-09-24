#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace afar::report {

struct RunReportInfo {
    std::string run_id;
    std::size_t completed_states{0};
    std::size_t valid_direct_count{0};
    /// Если пусто — в PDF пишется макрос сборки AFAR_SOFTWARE_VERSION.
    std::string software_version;
    std::string series_path;
    /// Пороги серии (настройки ПО, не аттестованная метрология).
    double max_drift_phase_deg{0};
    double max_phase_residual_deg{0};
};

/// Минимальный валидный PDF 1.4: серия, версии сборки, пороги, THRU.
bool writeRunReportPdf(const std::filesystem::path& path,
                       const RunReportInfo& info,
                       std::string& diagnostics);

/// Смоук: файл начинается с `%PDF-` и ненулевой.
bool isValidPdfSmoke(const std::filesystem::path& path, std::string& diagnostics);

}  // namespace afar::report
