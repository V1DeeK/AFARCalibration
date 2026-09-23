#pragma once

#include "afar/SweepTypes.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace afar {

struct RawS21StateRecord {
    std::vector<std::complex<double>> s21;
    float temperature_c{0.f};
    bool overload{false};
    /// Поточечная пригодность (не vector<bool> — избегаем проблем MinGW -Werror).
    std::vector<std::uint8_t> valid;
    std::uint16_t attempt{0};
    bool completed{false};
};

/// Собственный бинарный формат raw-s21.h5 без libhdf5 (DATA-006).
/// Magic: "AFARH5\\1", version 1. Один слот на (ch, att, phase).
class RawS21Store {
public:
    static constexpr char kMagic[8] = {'A', 'F', 'A', 'R', 'H', '5', '\1', '\0'};
    static constexpr std::uint32_t kVersion = 1;

    RawS21Store() = default;
    ~RawS21Store();

    RawS21Store(const RawS21Store&) = delete;
    RawS21Store& operator=(const RawS21Store&) = delete;
    RawS21Store(RawS21Store&&) noexcept;
    RawS21Store& operator=(RawS21Store&&) noexcept;

    /// Создаёт новый файл с осями и пустыми слотами (completed=false).
    static bool create(const std::filesystem::path& path,
                       const std::vector<std::uint8_t>& channels,
                       const std::vector<std::uint16_t>& att_codes,
                       const std::vector<std::uint8_t>& phase_codes,
                       const std::vector<std::uint64_t>& frequency_hz,
                       RawS21Store& out,
                       std::string& diagnostics);

    /// Открывает существующий файл для чтения/дозаписи (recovery).
    static bool open(const std::filesystem::path& path,
                     RawS21Store& out,
                     std::string& diagnostics);

    void close();

    [[nodiscard]] bool isOpen() const noexcept { return file_.is_open(); }
    [[nodiscard]] std::uint32_t nChannel() const noexcept { return n_channel_; }
    [[nodiscard]] std::uint32_t nAtt() const noexcept { return n_att_; }
    [[nodiscard]] std::uint32_t nPhase() const noexcept { return n_phase_; }
    [[nodiscard]] std::uint32_t nFreq() const noexcept { return n_freq_; }
    [[nodiscard]] const std::vector<std::uint8_t>& channels() const noexcept { return channels_; }
    [[nodiscard]] const std::vector<std::uint16_t>& attCodes() const noexcept { return att_codes_; }
    [[nodiscard]] const std::vector<std::uint8_t>& phaseCodes() const noexcept { return phase_codes_; }
    [[nodiscard]] const std::vector<std::uint64_t>& frequencyHz() const noexcept
    {
        return frequency_hz_;
    }

    [[nodiscard]] std::optional<std::size_t> indexOf(std::uint8_t channel,
                                                     std::uint16_t att_code,
                                                     std::uint8_t phase_code) const;

    /// Запись слота. Если completed==true — отказ (AT-05). Иначе можно переписать.
    bool writeState(std::uint8_t channel,
                    std::uint16_t att_code,
                    std::uint8_t phase_code,
                    const RawS21StateRecord& record,
                    std::string& diagnostics);

    bool readState(std::uint8_t channel,
                   std::uint16_t att_code,
                   std::uint8_t phase_code,
                   RawS21StateRecord& out,
                   std::string& diagnostics) const;

    [[nodiscard]] bool isCompleted(std::uint8_t channel,
                                   std::uint16_t att_code,
                                   std::uint8_t phase_code) const;

    /// Сброс флага completed для повторного измерения (FR-18). Данные слота сохраняются.
    bool clearCompleted(std::uint8_t channel,
                        std::uint16_t att_code,
                        std::uint8_t phase_code,
                        std::string& diagnostics);

    [[nodiscard]] std::size_t completedCount() const;

    /// Опора: слот [ch_idx][att_row] где att_row in 0..n_att (N_A+1).
    bool writeReference(std::uint8_t channel,
                        std::uint32_t att_row,
                        const std::vector<std::complex<double>>& s21,
                        std::string& diagnostics);

    bool readReference(std::uint8_t channel,
                       std::uint32_t att_row,
                       std::vector<std::complex<double>>& s21,
                       std::string& diagnostics) const;

    [[nodiscard]] std::size_t totalComplexSamplesCompleted() const;

private:
    mutable std::fstream file_;
    std::filesystem::path path_;
    std::uint32_t n_channel_{0};
    std::uint32_t n_att_{0};
    std::uint32_t n_phase_{0};
    std::uint32_t n_freq_{0};
    std::vector<std::uint8_t> channels_;
    std::vector<std::uint16_t> att_codes_;
    std::vector<std::uint8_t> phase_codes_;
    std::vector<std::uint64_t> frequency_hz_;
    std::uint64_t states_offset_{0};
    std::uint64_t refs_offset_{0};
    std::uint64_t state_stride_{0};
    std::uint64_t ref_stride_{0};

    [[nodiscard]] std::size_t flatIndex(std::size_t ch_i, std::size_t att_i, std::size_t ph_i) const;
    bool seekState(std::size_t flat) const;
    bool seekRef(std::size_t flat) const;
    bool writeRecordAt(std::size_t flat, const RawS21StateRecord& record, std::string& diagnostics);
    bool readRecordAt(std::size_t flat, RawS21StateRecord& out, std::string& diagnostics) const;
    static std::uint64_t computeStateStride(std::uint32_t n_freq);
    static std::uint64_t computeRefStride(std::uint32_t n_freq);
};

}  // namespace afar
