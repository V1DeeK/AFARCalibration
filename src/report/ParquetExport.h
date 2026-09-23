#pragma once

#include "AttenuatorCodes.h"
#include "DirectLut.h"
#include "RawS21Store.h"
#include "RunConfig.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace afar::report {

/// Строка обратной LUT (т. 7.4 / FR-14).
struct InverseLutEntry {
    std::uint8_t channel{};
    std::uint64_t freq_hz{};
    double target_atten_db{};
    double target_phase_deg{};
    std::uint16_t selected_att_code{};
    std::uint8_t selected_phase_code{};
    double measured_atten_db{};
    double measured_phase_deg{};
    double atten_residual_db{};
    double phase_residual_deg{};
    bool valid{false};
};

/// Magic columnar TSV без Apache Arrow: `AFARPQ\1` + UTF-8 TSV.
inline constexpr char kAfarPqMagic[7] = {'A', 'F', 'A', 'R', 'P', 'Q', '\1'};

bool exportDirectLut(const std::filesystem::path& path,
                     std::span<const cal::DirectLutEntry> rows,
                     std::string& diagnostics);

bool exportInverseLut(const std::filesystem::path& path,
                      std::span<const InverseLutEntry> rows,
                      std::string& diagnostics);

bool readDirectLut(const std::filesystem::path& path,
                   std::vector<cal::DirectLutEntry>& out,
                   std::string& diagnostics);

bool readInverseLut(const std::filesystem::path& path,
                    std::vector<InverseLutEntry>& out,
                    std::string& diagnostics);

/// Собирает прямую LUT из store (нормализация по опоре + unwrap).
bool buildDirectLutFromStore(const RawS21Store& store,
                             const RunConfig& config,
                             std::vector<cal::DirectLutEntry>& out,
                             std::string& diagnostics);

/// Обратная LUT: цели = att_cmd_db × phase_code·lsb по сетке direct.
bool buildInverseLutFromDirect(const std::vector<cal::DirectLutEntry>& direct,
                               const RunConfig& config,
                               const AttenuatorCodes& att,
                               std::vector<InverseLutEntry>& out,
                               std::string& diagnostics);

[[nodiscard]] std::size_t countValidDirect(std::span<const cal::DirectLutEntry> rows);
[[nodiscard]] std::size_t countValidInverse(std::span<const InverseLutEntry> rows);

}  // namespace afar::report
