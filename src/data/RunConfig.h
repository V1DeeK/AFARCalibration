#pragma once

#include "afar/SweepTypes.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace afar {

/// Разбор "S11"|"S12"|"S21"|"S22". false — неизвестное значение.
bool parseSParameter(std::string_view text, SParameter& out) noexcept;

struct VnaConfig {
    std::string model;
    std::string host;
    int port{};
    /// Один из S11|S12|S21|S22 (строка из JSON; в SweepConfig — через parseSParameter).
    std::string s_parameter;
    std::uint64_t f_start_hz{};
    std::uint64_t f_stop_hz{};
    int points{};
    int ifbw_hz{};
    double power_dbm{};
    int averages{};
};

struct ControllerConfig {
    std::string driver;
    std::string endpoint;
};

struct ChannelRange {
    int first{};
    int last{};
};

struct PhaseCodeRange {
    int first{};
    int last{};
    double lsb_deg{};
};

struct ReferenceState {
    int att_code{};
    int phase_code{};
};

struct DutConfig {
    std::string serial;
    ChannelRange channels;
    PhaseCodeRange phase_codes;
    std::string attenuator_codes_file;
    ReferenceState reference;
};

struct TimingConfig {
    int settle_ms{};
    bool reference_after_phase_row{};
};

struct LimitsConfig {
    double max_drift_phase_deg{};
    double max_phase_residual_deg{};
};

/// Конфигурация серии схемы afar.stage1.run-config/v1 (DATA-003, FR-02).
struct RunConfig {
    static constexpr std::string_view kSchemaName = "afar.stage1.run-config/v1";

    std::string schema;
    std::string run_id;
    VnaConfig vna;
    ControllerConfig controller;
    DutConfig dut;
    TimingConfig timing;
    LimitsConfig limits;

    /// Чтение UTF-8 JSON и проверка до любого обращения к железу (AT-02).
    /// При ошибке возвращает false и заполняет diagnostics.
    static bool loadFromFile(const std::filesystem::path& path,
                             RunConfig& out,
                             std::string& diagnostics);

    /// Разбор уже прочитанного UTF-8 текста.
    static bool parse(std::string_view utf8_json,
                      RunConfig& out,
                      std::string& diagnostics);
};

}  // namespace afar
