#pragma once

#include "IScpiTransport.h"

#include "afar/IVna.h"
#include "afar/ScpiIdn.h"

#include <cstdint>
#include <string>
#include <vector>

/// Драйвер двухпортовых ПЛАНАР C1220/C2220 / S2VNA поверх IScpiTransport.
/// Только SCPI из docs/contracts/vna-c2220-scpi.md.
class C2220Vna final : public IVna {
public:
    struct Profile {
        /// Явное подтверждение режима прямого доступа в профиле стенда (HW-VNA-05).
        bool allow_direct_access{false};
        /// Подстрока модели в ответе *IDN? (по умолчанию C2220).
        std::string required_model{"C2220"};
        std::uint32_t connect_timeout_ms{3000};
        std::uint32_t sweep_timeout_ms{30000};
        int measure_retries{2};
    };

    explicit C2220Vna(IScpiTransport& transport);
    C2220Vna(IScpiTransport& transport, Profile profile);

    void connect() override;
    std::string identify() override;
    /// Последний успешный `identify()` (пустая строка до первого вызова).
    const ScpiIdnFields& last_idn() const noexcept { return last_idn_; }
    void configure(const SweepConfig& config) override;
    SweepConfig read_config() override;
    ComplexSweep measure_trace() override;
    ComplexSweep measure_s21() override;
    /// Считать уже отображаемую в S2VNA трассу без изменения диапазона и нового trigger.
    ComplexSweep read_current_trace(SweepConfig* instrument_config = nullptr);
    struct TriggerState {
        std::string source;
        bool continuous{false};
    };
    /// Показ графика S2VNA: запомнить текущий запуск и включить непрерывную развертку.
    TriggerState enable_native_continuous();
    void restore_native_trigger(const TriggerState& previous);
    /// SCPI-состояние S2VNA; содержит настройки каналов и может содержать калибровку.
    void load_state(const std::string& path);
    /// Сохранить полный снимок S2VNA для отката, не меняя постоянный тип сохранения.
    void save_state(const std::string& path);
    /// Применить настройки непрерывного свипа, не меняя трассы и источник запуска.
    SweepConfig apply_native_sweep(const SweepConfig& config);
    struct NativeOperatingState {
        bool rf_output{false};
        bool correction{false};
    };
    NativeOperatingState native_operating_state();
    NativeOperatingState set_native_operating_state(bool rf_output, bool correction);
    struct NativeTrace {
        SParameter parameter{SParameter::S11};
        std::string format{"MLOG"};
    };
    struct NativeGraphState {
        std::vector<std::string> parameters;
        std::vector<std::string> formats;
        int layout{1};
    };
    NativeGraphState native_graph_state();
    void configure_native_graph(const std::vector<NativeTrace>& traces, bool separate);
    /// Вернуть трассы/форматы/раскладку после разового измерения для отчёта.
    void restore_native_graph_state(const NativeGraphState& state);
    void auto_scale_native_graph();
    struct NativeMarker {
        double frequency_hz{0.0};
        double value{0.0};
        double secondary{0.0};
    };
    int native_marker_count(int trace);
    bool native_reference_marker_active(int trace);
    void set_native_marker_count(int trace, int count);
    std::vector<NativeMarker> native_markers(int trace);
    std::vector<std::string> native_trace_parameters();
    void set_native_trace_parameter(int trace, SParameter parameter);
    /// Текущий комплект мер S2VNA, например `CKIT 1 (85032F)`.
    std::string current_calibration_kit_id();
    /// Выбрать один из комплектов мер S2VNA 1..64 и вернуть его номер/метку.
    std::string select_calibration_kit(int index);
    /// Подкласс мер (1=OPEN-M, 2=OPEN-F для комплекта 6650F27).
    void select_calibration_subclass(int subclass);
    /// Включена ли коррекция на активном канале S2VNA.
    bool correction_enabled();
    void calibrate_one_port(OnePortCalibrationStep step, int port) override;
    void calibrate_two_port(TwoPortCalibrationStep step) override;
    std::vector<std::string> drain_errors() override;
    void abort() noexcept override;

    bool connected() const noexcept { return connected_; }
    const Profile& profile() const noexcept { return profile_; }

private:
    void require_connected(const char* op) const;
    void write_cmd(const std::string& cmd);
    std::string query(const std::string& cmd);
    void check_direct_access();
    void reject_if_foreign_model(const std::string& idn) const;
    ComplexSweep measure_once();
    ComplexSweep read_trace_data(SParameter parameter, std::uint32_t expected_points);

    IScpiTransport& transport_;
    Profile profile_;
    bool connected_{false};
    SweepConfig config_{};
    bool configured_{false};
    ScpiIdnFields last_idn_{};
};
