#include "ScanOrder.h"

namespace afar {

ScanOrder::ScanOrder(const RunConfig& config, const AttenuatorCodes& att_codes)
{
    for (int ch = config.dut.channels.first; ch <= config.dut.channels.last; ++ch) {
        channels_.push_back(static_cast<std::uint8_t>(ch));
    }
    for (const auto& row : att_codes.rows) {
        if (row.enabled) {
            enabled_att_.push_back(static_cast<std::uint16_t>(row.att_code));
        }
    }
    for (int ph = config.dut.phase_codes.first; ph <= config.dut.phase_codes.last; ++ph) {
        phase_codes_.push_back(static_cast<std::uint8_t>(ph));
    }

    for (const auto ch : channels_) {
        for (const auto att : enabled_att_) {
            for (std::size_t pi = 0; pi < phase_codes_.size(); ++pi) {
                ScanItem item;
                item.state.channel = ch;
                item.state.att_code = att;
                item.state.phase_code = phase_codes_[pi];
                const bool last_phase = (pi + 1 == phase_codes_.size());
                item.need_reference = last_phase && config.timing.reference_after_phase_row;
                items_.push_back(item);
            }
        }
    }
}

std::optional<std::uint32_t> ScanOrder::referenceAttRow(std::uint16_t att_code) const
{
    for (std::uint32_t i = 0; i < enabled_att_.size(); ++i) {
        if (enabled_att_[i] == att_code) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace afar
