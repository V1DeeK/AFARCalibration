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
    /// Полный *IDN? и разобранные поля (AT-01).
    std::string vna_idn;
    std::string vna_model;
    std::string vna_serial;
    std::string vna_firmware;
    /// Пороги серии (настройки ПО, не аттестованная метрология).
    double max_drift_phase_deg{0};
    double max_phase_residual_deg{0};

    /// FR-05 / thru-approval.json: пороги и утверждение метролога (не аттестация).
    double thru_limit_mag_db{0.20};
    double thru_limit_phase_deg{2.0};
    double thru_measured_mag_db{-1.0};   ///< <0 — не задано
    double thru_measured_phase_deg{-1.0};
    bool metrologist_approved{false};
    std::string metrologist_name;
    std::string metrologist_date;
    bool thru_meta_loaded{false};
};

/// Минимальный валидный PDF 1.4: серия, версии сборки, пороги, THRU, метролог.
bool writeRunReportPdf(const std::filesystem::path& path,
                       const RunReportInfo& info,
                       std::string& diagnostics);

/// Смоук: файл начинается с `%PDF-` и ненулевой.
bool isValidPdfSmoke(const std::filesystem::path& path, std::string& diagnostics);

/// Читает thru-approval.json (из мастера) в поля FR-05; false — файла нет / разбор не удался.
bool loadThruApprovalJson(const std::filesystem::path& path, RunReportInfo& info);

}  // namespace afar::report
