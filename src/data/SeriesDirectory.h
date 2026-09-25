#pragma once

#include <filesystem>
#include <string>

namespace afar {

/// Каталог серии `<root>/<run_id>/` (DATA-005, т. 7.1).
class SeriesDirectory {
public:
    static constexpr const char* kRunConfig = "run-config.json";
    static constexpr const char* kAttenuatorCodes = "attenuator-codes.csv";
    static constexpr const char* kThruApproval = "thru-approval.json";
    static constexpr const char* kRawS21 = "raw-s21.h5";
    /// Табличный экспорт т. 7.3; не подменяет kRawS21.
    static constexpr const char* kRawS21Csv = "raw-s21.csv";
    static constexpr const char* kRunEvents = "run-events.jsonl";
    static constexpr const char* kDirectLut = "direct-lut.parquet";
    static constexpr const char* kInverseLut = "inverse-lut.parquet";
    static constexpr const char* kReport = "report.pdf";
    static constexpr const char* kManifest = "manifest.sha256";

    /// Создаёт каталог, копирует immutable run-config.json и attenuator-codes.csv.
    /// Запрещает escape за пределы `<root>/<run_id>`.
    static bool create(const std::filesystem::path& data_root,
                       const std::string& run_id,
                       const std::filesystem::path& run_config_src,
                       const std::filesystem::path& attenuator_csv_src,
                       SeriesDirectory& out,
                       std::string& diagnostics);

    /// Открывает уже существующий каталог серии (recovery).
    static bool openExisting(const std::filesystem::path& series_path,
                             SeriesDirectory& out,
                             std::string& diagnostics);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
    [[nodiscard]] const std::string& runId() const noexcept { return run_id_; }

    [[nodiscard]] std::filesystem::path runConfigPath() const;
    [[nodiscard]] std::filesystem::path attenuatorCodesPath() const;
    [[nodiscard]] std::filesystem::path rawS21Path() const;
    [[nodiscard]] std::filesystem::path rawS21CsvPath() const;
    [[nodiscard]] std::filesystem::path runEventsPath() const;
    [[nodiscard]] std::filesystem::path directLutPath() const;
    [[nodiscard]] std::filesystem::path inverseLutPath() const;
    [[nodiscard]] std::filesystem::path reportPath() const;
    [[nodiscard]] std::filesystem::path manifestPath() const;

    /// Проверяет, что relative остаётся внутри каталога серии.
    [[nodiscard]] bool isInside(const std::filesystem::path& candidate) const;

private:
    std::filesystem::path root_;
    std::string run_id_;

    [[nodiscard]] std::filesystem::path joinChecked(const char* filename) const;
};

}  // namespace afar
