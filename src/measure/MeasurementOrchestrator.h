#pragma once

#include "AttenuatorCodes.h"
#include "RawS21Store.h"
#include "RunConfig.h"
#include "RunEventLog.h"
#include "RunStateMachine.h"
#include "ScanOrder.h"
#include "SeriesDirectory.h"
#include "afar/IDutController.h"
#include "afar/IVna.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace afar {

/// Оркестратор измерения (CORE-004…008).
class MeasurementOrchestrator {
public:
    /// HW-VNA-04: число повторов после первого сбоя (итого попыток = 1 + retries).
    static constexpr int kVnaRetries = 2;

    MeasurementOrchestrator(IVna* vna, IDutController* dut);

    /// Копия конфигурации + коды аттенюатора до железа.
    void setConfig(RunConfig config, AttenuatorCodes att_codes);

    /// Создаёт серию, store, log; Idle→…→Ready. Проверяет IDN содержит C2220.
    /// \p vna_calibration_id — ручной id из мастера (RMD-004); пустой допустим.
    bool prepare(const std::filesystem::path& data_root,
                 const std::filesystem::path& run_config_src,
                 const std::filesystem::path& attenuator_csv_src,
                 std::string& diagnostics,
                 const std::string& vna_calibration_id = {});

    /// Recovery: открыть существующую серию и store (AT-06).
    bool prepareRecovery(const std::filesystem::path& series_dir,
                         std::string& diagnostics);

    bool start(std::string& diagnostics);
    bool pause();
    bool resume(std::string& diagnostics);
    bool stop();

    /// FR-18: сброс completed для фаз и сдвиг cursor на первый incomplete.
    /// Допустимо из Ready или Paused (не из Running).
    /// Атомарно: при ошибке clearCompleted откатывает уже сброшенные фазы.
    bool requestRemeasure(std::uint8_t channel,
                          std::uint16_t att_code,
                          const std::vector<std::uint8_t>& phases,
                          std::string& diagnostics);

    /// Синхронный прогон до Complete / Paused / Aborted / Error (тесты).
    void runUntilDone();

    /// Один шаг измерения (или переход паузы/стопа). false — нечего делать.
    bool stepOnce();

    [[nodiscard]] RunState state() const noexcept { return sm_.state(); }
    [[nodiscard]] RawS21Store& store() { return store_; }
    [[nodiscard]] const RawS21Store& store() const { return store_; }
    [[nodiscard]] SeriesDirectory& series() { return series_; }
    [[nodiscard]] const SeriesDirectory& series() const { return series_; }
    [[nodiscard]] RunEventLog& eventLog() { return log_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return last_error_; }
    [[nodiscard]] const ScanOrder& scanOrder() const noexcept { return scan_; }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
    [[nodiscard]] bool hasLastMeasuredSweep() const noexcept { return has_last_sweep_; }
    [[nodiscard]] const ComplexSweep& lastMeasuredSweep() const noexcept { return last_sweep_; }

    /// Отключить sleep settle (ускорение тестов при ненулевом settle_ms).
    void setSleepEnabled(bool enabled) noexcept { sleep_enabled_ = enabled; }

    /// GAP-LAYER-001: проверка связи из Idle (connect + identify), без нового состояния.
    /// Отказ (в т.ч. чужая модель) — abort VNA; запись в журнал, если он открыт.
    bool probeIdentify(std::string& idn_or_diagnostics);

    /// GAP-WIZ-001: короткий пробный съём из Idle (канал 1, att 0 и att_last, фазы 0 и phase_last).
    /// Без серии и без смены автомата. Отмена мастера этот метод не вызывает.
    bool runProbeCodes(const SweepConfig& sweep,
                       std::uint16_t att_last,
                       std::uint8_t phase_last,
                       std::string& diagnostics);

private:
    IVna* vna_{nullptr};
    IDutController* dut_{nullptr};
    RunConfig config_{};
    AttenuatorCodes att_codes_{};
    SeriesDirectory series_{};
    RawS21Store store_;
    RunEventLog log_;
    RunStateMachine sm_;
    ScanOrder scan_;
    std::size_t cursor_{0};
    bool pause_requested_{false};
    bool stop_requested_{false};
    bool sleep_enabled_{true};
    std::string last_error_;
    std::vector<std::uint64_t> frequency_axis_;
    ComplexSweep last_sweep_{};
    bool has_last_sweep_{false};

    /// `stand` — снимок кодов, если он ещё приложен к тракту. Температура читается отдельно, если связь жива.
    bool transitionLogged(RunState target,
                          const std::string& reason,
                          const DutState* stand = nullptr);
    void logEvent(EventLevel level,
                  const std::string& code,
                  const std::string& text,
                  const DutState* st = nullptr,
                  std::optional<int> attempt = std::nullopt);
    bool ensureHardwareReady(std::string& diagnostics);
    bool measureOneState(const ScanItem& item);
    bool measureReference(const ScanItem& after_item);
    /// HW-VNA-03: снять очередь SYST:ERR? и записать непустые строки в JSONL.
    void drainAndLogVnaErrors(const DutState* st = nullptr);
    /// HW-DUT-03: readback или выдержка; false — расхождение/отказ (уже в Error).
    bool confirmDutOrSettle(const DutState& expected);
    int settleMsFor(std::uint16_t att_code) const;
    void doSleep(int ms) const;
    bool buildFrequencyAxis();
    std::size_t findFirstIncomplete() const;
    /// Finalizing: LUT Parquet, PDF (манифест — отдельно, после строки Complete в журнале).
    bool finalizeExports(std::string& diagnostics);
};

}  // namespace afar
