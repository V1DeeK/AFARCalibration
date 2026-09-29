#pragma once

#include "RawS21Store.h"

#include <filesystem>
#include <string>

namespace afar::report {

/// Табличный экспорт сырья т. 7.3 / data-formats §8. Имя не подменяет raw-s21.h5.
inline constexpr const char* kRawS21CsvName = "raw-s21.csv";

/// CSV с колонками контракта. timestamp_utc — из последнего STATE_OK журнала слота.
bool exportRawS21Csv(const std::filesystem::path& path,
                     const RawS21Store& store,
                     const std::string& run_id,
                     const std::filesystem::path& run_events_path,
                     std::string& diagnostics);

}  // namespace afar::report
