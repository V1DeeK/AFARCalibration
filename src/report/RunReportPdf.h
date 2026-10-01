#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace afar::report {

struct ChannelReportInfo {
    std::uint8_t channel{0};
    std::size_t completed_states{0};
    std::size_t valid_direct_count{0};
    std::size_t valid_inverse_count{0};
};

struct RunReportInfo {
    std::string run_id;
    std::size_t completed_states{0};
    std::size_t valid_direct_count{0};
    std::size_t valid_inverse_count{0};
    std::size_t frequency_points{0};
    std::size_t attenuator_codes{0};
    std::size_t phase_codes{0};
    std::uint64_t f_start_hz{0};
    std::uint64_t f_stop_hz{0};
    std::vector<ChannelReportInfo> channels;
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

/// PDF серии: параметры, версии, пороги и отдельная сводка каждого канала.
bool writeRunReportPdf(const std::filesystem::path& path,
                       const RunReportInfo& info,
                       std::string& diagnostics);

/// Смоук: файл начинается с `%PDF-` и ненулевой.
bool isValidPdfSmoke(const std::filesystem::path& path, std::string& diagnostics);

/// Читает thru-approval.json (из мастера) в поля FR-05; false — файла нет / разбор не удался.
bool loadThruApprovalJson(const std::filesystem::path& path, RunReportInfo& info);

}  // namespace afar::report
