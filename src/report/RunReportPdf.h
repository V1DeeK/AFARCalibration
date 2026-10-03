#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace afar::report {

struct ChannelStateReport {
    std::uint16_t att_code{0};
    std::uint8_t phase_code{0};
    bool completed{false};
    bool valid{false};
    bool overload{false};
    std::uint16_t attempt{0};
    std::size_t sample_count{0};
    std::size_t valid_sample_count{0};
    double center_atten_db{0.0};
    double center_phase_error_deg{0.0};
    bool center_value_available{false};
    std::uint64_t worst_frequency_hz{0};
    double worst_atten_error_db{0.0};
    double worst_phase_error_deg{0.0};
};

struct WorstStateReport {
    std::uint16_t att_code{0};
    std::uint8_t phase_code{0};
    std::uint64_t frequency_hz{0};
    double atten_error_db{0.0};
    double phase_error_deg{0.0};
    bool valid{false};
};

struct ChannelReportInfo {
    std::uint8_t channel{0};
    std::size_t expected_states{0};
    std::size_t completed_states{0};
    std::size_t valid_states{0};
    std::size_t error_states{0};
    std::size_t valid_direct_count{0};
    std::size_t valid_inverse_count{0};
    std::size_t invalid_direct_count{0};
    std::size_t overload_states{0};
    std::size_t retry_count{0};
    double max_atten_error_db{0.0};
    double max_phase_error_deg{0.0};
    double max_repeatability_db{0.0};
    double max_repeatability_deg{0.0};
    double max_drift_phase_deg{0.0};
    std::uint64_t center_frequency_hz{0};
    std::vector<std::uint16_t> att_codes;
    std::vector<std::uint8_t> phase_codes;
    std::vector<ChannelStateReport> states;
    std::vector<WorstStateReport> worst_states;
    std::string calibration_filename;
    std::string calibration_sha256;
    std::string calibration_csv_filename;
    std::string calibration_csv_sha256;
    std::string raw_filename;
    std::string raw_sha256;
};

struct RunReportInfo {
    bool detailed_channel_report{false};
    std::string run_id;
    std::size_t completed_states{0};
    std::size_t valid_direct_count{0};
    std::size_t valid_inverse_count{0};
    std::size_t frequency_points{0};
    std::size_t attenuator_codes{0};
    std::size_t phase_codes{0};
    std::uint64_t f_start_hz{0};
    std::uint64_t f_stop_hz{0};
    std::uint32_t ifbw_hz{0};
    double power_dbm{0.0};
    std::uint16_t averages{0};
    std::vector<ChannelReportInfo> channels;
    /// Если пусто — в PDF пишется макрос сборки AFAR_SOFTWARE_VERSION.
    std::string software_version;
    std::string series_path;
    /// Полный *IDN? и разобранные поля (AT-01).
    std::string vna_idn;
    std::string vna_model;
    std::string vna_serial;
    std::string vna_firmware;
    bool demo_mode{false};
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

/// PASS допустим только при полном покрытии и отсутствии ошибочных состояний.
[[nodiscard]] bool channelReportPasses(const ChannelReportInfo& channel) noexcept;

/// PDF серии: параметры, версии, пороги и отдельная сводка каждого канала.
bool writeRunReportPdf(const std::filesystem::path& path,
                       const RunReportInfo& info,
                       std::string& diagnostics);

/// Смоук: файл начинается с `%PDF-` и ненулевой.
bool isValidPdfSmoke(const std::filesystem::path& path, std::string& diagnostics);

/// Читает thru-approval.json (из мастера) в поля FR-05; false — файла нет / разбор не удался.
bool loadThruApprovalJson(const std::filesystem::path& path, RunReportInfo& info);

}  // namespace afar::report
