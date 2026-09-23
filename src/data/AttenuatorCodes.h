#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace afar {

struct AttenuatorCodeRow {
    int att_code{};
    double att_cmd_db{};  // номинальная подпись, не измерение
    bool enabled{};
    int settle_ms{};
};

/// Таблица кодов аттенюатора из CSV (DATA-004, т. 6.2).
struct AttenuatorCodes {
    static constexpr std::string_view kHeader = "att_code,att_cmd_db,enabled,settle_ms";

    std::vector<AttenuatorCodeRow> rows;

    /// Число строк с enabled=true (N_A).
    [[nodiscard]] std::size_t enabledCount() const;

    static bool loadFromFile(const std::filesystem::path& path,
                             AttenuatorCodes& out,
                             std::string& diagnostics);

    static bool parse(std::string_view utf8_csv,
                      AttenuatorCodes& out,
                      std::string& diagnostics);
};

}  // namespace afar
