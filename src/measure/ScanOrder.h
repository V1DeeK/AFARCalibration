#pragma once

#include "AttenuatorCodes.h"
#include "RunConfig.h"
#include "afar/IDutController.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace afar {

struct ScanItem {
    DutState state{};
    /// После этой точки фазы (конец строки phase) нужна опора.
    bool need_reference{false};
};

/// Итератор состояний: channel → enabled att → phase (CORE-003).
class ScanOrder {
public:
    ScanOrder() = default;
    ScanOrder(const RunConfig& config, const AttenuatorCodes& att_codes);

    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] const ScanItem& at(std::size_t i) const { return items_[i]; }
    [[nodiscard]] const std::vector<ScanItem>& items() const noexcept { return items_; }

    [[nodiscard]] const std::vector<std::uint8_t>& channels() const noexcept { return channels_; }
    [[nodiscard]] const std::vector<std::uint16_t>& enabledAttCodes() const noexcept
    {
        return enabled_att_;
    }
    [[nodiscard]] const std::vector<std::uint8_t>& phaseCodes() const noexcept
    {
        return phase_codes_;
    }

    /// Индекс строки опоры для att: 0..N_A (att_i после строки phase → att_i).
    [[nodiscard]] std::optional<std::uint32_t> referenceAttRow(std::uint16_t att_code) const;

private:
    std::vector<ScanItem> items_;
    std::vector<std::uint8_t> channels_;
    std::vector<std::uint16_t> enabled_att_;
    std::vector<std::uint8_t> phase_codes_;
};

}  // namespace afar
