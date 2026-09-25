#include "MeasurementOrchestrator.h"

#include "Manifest.h"
#include "ParquetExport.h"
#include "QualityGates.h"
#include "RawS21TableExport.h"
#include "RunReportPdf.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

namespace afar {
namespace {

bool idnContainsC2220(const std::string& idn)
{
    return idn.find("C2220") != std::string::npos;
}

bool hasNanOrInf(const ComplexSweep& sweep)
{
    for (const auto& z : sweep.s21) {
        if (!std::isfinite(z.real()) || !std::isfinite(z.imag())) {
            return true;
        }
    }
    return false;
}

DutState appliedStand(const ScanItem& item, const RunConfig& config)
{
    DutState stand = item.state;
    if (item.need_reference) {
        stand.att_code = static_cast<std::uint16_t>(config.dut.reference.att_code);
        stand.phase_code = static_cast<std::uint8_t>(config.dut.reference.phase_code);
    }
    return stand;
}

bool codesMatch(const DutState& a, const DutState& b) noexcept
{
    return a.channel == b.channel && a.att_code == b.att_code
        && a.phase_code == b.phase_code;
}

/// Нет обратного чтения / протокол не передан — остаётся выдержка профиля (HW-DUT-03).
bool isNoReadbackException(const std::string& msg)
{
    return msg.find("нет обратного чтения") != std::string::npos
        || msg.find("protocol not provided") != std::string::npos
        || msg.find("протокол") != std::string::npos
        || msg.find("не передан") != std::string::npos;
}

SweepConfig sweepConfigFromRun(const RunConfig& config)
{
    SweepConfig sweep{};
    sweep.f_start_hz = config.vna.f_start_hz;
    sweep.f_stop_hz = config.vna.f_stop_hz;
    sweep.points = static_cast<std::uint32_t>(config.vna.points);
    sweep.power_dbm = config.vna.power_dbm;
    sweep.ifbw_hz = static_cast<std::uint32_t>(config.vna.ifbw_hz);
    sweep.averages = static_cast<std::uint16_t>(config.vna.averages);
    // s_parameter перезаписывается в measureAllFour; парсим для согласованности с prepare.
    (void)parseSParameter(config.vna.s_parameter, sweep.s_parameter);
    return sweep;
}

/// DATA-102: четыре trace через полный configure (публичного DEF-only в C2220Vna нет).
ComplexSweep measureAllFour(IVna& vna, SweepConfig base)
{
    ComplexSweep out;
    constexpr SParameter order[] = {
        SParameter::S11,
        SParameter::S21,
        SParameter::S12,
        SParameter::S22,
    };
    for (const auto p : order) {
        base.s_parameter = p;
        vna.configure(base);
        auto part = vna.measure_trace();
        if (out.frequency_hz.empty()) {
            out.frequency_hz = std::move(part.frequency_hz);
        }
        out.overload = out.overload || part.overload;
        switch (p) {
        case SParameter::S11:
            out.s11 = std::move(part.s11);
            break;
        case SParameter::S21:
            out.s21 = std::move(part.s21);
            break;
        case SParameter::S12:
            out.s12 = std::move(part.s12);
            break;
        case SParameter::S22:
            out.s22 = std::move(part.s22);
            break;
        }
    }
    return out;
}

}  // namespace

MeasurementOrchestrator::MeasurementOrchestrator(IVna* vna, IDutController* dut)
    : vna_(vna)
    , dut_(dut)
{
}

void MeasurementOrchestrator::setConfig(RunConfig config, AttenuatorCodes att_codes)
{
    config_ = std::move(config);
    att_codes_ = std::move(att_codes);
    scan_ = ScanOrder(config_, att_codes_);
}

void MeasurementOrchestrator::logEvent(EventLevel level,
                                       const std::string& code,
                                       const std::string& text,
                                       const DutState* st,
                                       std::optional<int> attempt)
{
    if (!log_.isOpen()) {
        return;
    }
    RunEvent ev;
    ev.timestamp_utc = RunEventLog::nowUtcIso8601();
    ev.level = level;
    ev.component = "measurement-core";
    ev.event_code = code;
    ev.run_id = config_.run_id;
    if (st) {
        ev.channel = static_cast<int>(st->channel);
        ev.att_code = static_cast<int>(st->att_code);
        ev.phase_code = static_cast<int>(st->phase_code);
    }
    ev.attempt = attempt;
    ev.text = text;
    std::string diag;
    (void)log_.append(ev, diag);
}

bool MeasurementOrchestrator::transitionLogged(RunState target,
                                               const std::string& reason,
                                               const DutState* stand)
{
    const auto from = sm_.state();
    if (!sm_.tryTransition(target)) {
        logEvent(EventLevel::Warning, "STATE_REJECT",
                 std::string("rejected ") + toString(from) + " -> " + toString(target)
                     + ": " + reason);
        return false;
    }
    // Автотест не задаёт оператора: в строке явное «нет оператора», не пустое поле.
    std::string text = std::string(toString(from)) + " -> " + toString(target) + ": " + reason
        + "; пользователь: нет оператора";
    if (stand != nullptr) {
        text += "; channel=" + std::to_string(stand->channel);
        text += " att_code=" + std::to_string(stand->att_code);
        text += " phase_code=" + std::to_string(stand->phase_code);
    }
    if (dut_ != nullptr) {
        try {
            const double temperature_c = dut_->temperature_c();
            std::ostringstream oss;
            oss << "; temperature_c=" << temperature_c;
            text += oss.str();
        } catch (const std::exception&) {
            // Связи нет — снимок температуры не выдумываем.
        }
    }
    logEvent(EventLevel::Info, "STATE_TRANSITION", text, stand);
    return true;
}

bool MeasurementOrchestrator::buildFrequencyAxis()
{
    frequency_axis_.clear();
    const auto n = static_cast<std::uint32_t>(config_.vna.points);
    if (n == 0) {
        return false;
    }
    frequency_axis_.resize(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t f = config_.vna.f_start_hz;
        if (n > 1) {
            const auto span = config_.vna.f_stop_hz - config_.vna.f_start_hz;
            f = config_.vna.f_start_hz
                + (span * static_cast<std::uint64_t>(i))
                      / static_cast<std::uint64_t>(n - 1);
        }
        frequency_axis_[i] = f;
    }
    return true;
}

bool MeasurementOrchestrator::ensureHardwareReady(std::string& diagnostics)
{
    try {
        vna_->connect();
        dut_->connect();
        const auto idn = vna_->identify();
        if (!idnContainsC2220(idn)) {
            diagnostics = "VNA IDN does not contain C2220: " + idn;
            return false;
        }
        SweepConfig sweep{};
        sweep.f_start_hz = config_.vna.f_start_hz;
        sweep.f_stop_hz = config_.vna.f_stop_hz;
        sweep.points = static_cast<std::uint32_t>(config_.vna.points);
        sweep.power_dbm = config_.vna.power_dbm;
        sweep.ifbw_hz = static_cast<std::uint32_t>(config_.vna.ifbw_hz);
        sweep.averages = static_cast<std::uint16_t>(config_.vna.averages);
        if (!parseSParameter(config_.vna.s_parameter, sweep.s_parameter)) {
            diagnostics = "vna.s_parameter: must be one of S11|S12|S21|S22";
            return false;
        }
        vna_->configure(sweep);
        drainAndLogVnaErrors();
        return true;
    } catch (const std::exception& ex) {
        drainAndLogVnaErrors();
        diagnostics = ex.what();
        return false;
    }
}

bool MeasurementOrchestrator::prepare(const std::filesystem::path& data_root,
                                      const std::filesystem::path& run_config_src,
                                      const std::filesystem::path& attenuator_csv_src,
                                      std::string& diagnostics,
                                      const std::string& vna_calibration_id)
{
    diagnostics.clear();
    last_error_.clear();
    if (!vna_ || !dut_) {
        diagnostics = "VNA/DUT pointers are null";
        return false;
    }
    if (scan_.size() == 0) {
        diagnostics = "scan order is empty";
        return false;
    }
    if (!buildFrequencyAxis()) {
        diagnostics = "invalid frequency axis";
        return false;
    }

    if (!transitionLogged(RunState::Connecting, "prepare")) {
        diagnostics = "cannot leave Idle";
        return false;
    }

    if (!SeriesDirectory::create(data_root, config_.run_id, run_config_src, attenuator_csv_src,
                                 series_, diagnostics)) {
        (void)transitionLogged(RunState::Error, diagnostics);
        return false;
    }
    if (!RunEventLog::openAppend(series_.runEventsPath(), log_, diagnostics)) {
        (void)transitionLogged(RunState::Error, diagnostics);
        return false;
    }

    if (!ensureHardwareReady(diagnostics)) {
        last_error_ = diagnostics;
        logEvent(EventLevel::Error, "HW_CONNECT_FAIL", diagnostics);
        (void)transitionLogged(RunState::Error, diagnostics);
        return false;
    }

    if (!transitionLogged(RunState::SelfTest, "connected")) {
        diagnostics = "self-test transition failed";
        return false;
    }

    std::string store_diag;
    RawS21Meta store_meta;
    {
        std::ifstream cfg_in(series_.runConfigPath(), std::ios::binary);
        if (!cfg_in) {
            diagnostics = "cannot read applied run-config for raw meta";
            last_error_ = diagnostics;
            (void)transitionLogged(RunState::Error, diagnostics);
            return false;
        }
        store_meta.run_config_json.assign(std::istreambuf_iterator<char>(cfg_in),
                                          std::istreambuf_iterator<char>());
    }
    try {
        store_meta.vna_idn = vna_->identify();
    } catch (const std::exception& ex) {
        diagnostics = ex.what();
        last_error_ = diagnostics;
        (void)transitionLogged(RunState::Error, diagnostics);
        return false;
    }
    // RMD-004 / CAL-001: ручной id из мастера; SCPI калибровки нет.
    store_meta.vna_calibration_id = vna_calibration_id;
    if (!RawS21Store::create(series_.rawS21Path(), scan_.channels(), scan_.enabledAttCodes(),
                             scan_.phaseCodes(), frequency_axis_, store_meta, store_,
                             store_diag)) {
        diagnostics = store_diag;
        last_error_ = diagnostics;
        (void)transitionLogged(RunState::Error, diagnostics);
        return false;
    }

    if (!transitionLogged(RunState::Ready, "store ready")) {
        diagnostics = "ready transition failed";
        return false;
    }
    cursor_ = 0;
    pause_requested_ = false;
    stop_requested_ = false;
    return true;
}

bool MeasurementOrchestrator::prepareRecovery(const std::filesystem::path& series_dir,
                                              std::string& diagnostics)
{
    diagnostics.clear();
    last_error_.clear();
    if (!SeriesDirectory::openExisting(series_dir, series_, diagnostics)) {
        return false;
    }
    if (!RunConfig::loadFromFile(series_.runConfigPath(), config_, diagnostics)) {
        return false;
    }
    if (!AttenuatorCodes::loadFromFile(series_.attenuatorCodesPath(), att_codes_, diagnostics)) {
        return false;
    }
    scan_ = ScanOrder(config_, att_codes_);
    if (!buildFrequencyAxis()) {
        diagnostics = "invalid frequency axis";
        return false;
    }
    if (!RunEventLog::openAppend(series_.runEventsPath(), log_, diagnostics)) {
        return false;
    }
    if (!RawS21Store::open(series_.rawS21Path(), store_, diagnostics)) {
        return false;
    }
    if (!ensureHardwareReady(diagnostics)) {
        last_error_ = diagnostics;
        return false;
    }

    // Reset SM to Ready via Idle path for recovery entry.
    sm_ = RunStateMachine{};
    if (!transitionLogged(RunState::Connecting, "recovery")
        || !transitionLogged(RunState::SelfTest, "recovery")
        || !transitionLogged(RunState::Ready, "recovery")) {
        diagnostics = "cannot reach Ready for recovery";
        return false;
    }
    cursor_ = findFirstIncomplete();
    pause_requested_ = false;
    stop_requested_ = false;
    logEvent(EventLevel::Info, "RECOVERY",
             "resume from incomplete index " + std::to_string(cursor_));
    return true;
}

std::size_t MeasurementOrchestrator::findFirstIncomplete() const
{
    for (std::size_t i = 0; i < scan_.size(); ++i) {
        const auto& st = scan_.at(i).state;
        if (!store_.isCompleted(st.channel, st.att_code, st.phase_code)) {
            return i;
        }
    }
    return scan_.size();
}

bool MeasurementOrchestrator::start(std::string& diagnostics)
{
    diagnostics.clear();
    if (sm_.state() != RunState::Ready && sm_.state() != RunState::Paused) {
        diagnostics = "start requires Ready or Paused";
        return false;
    }
    if (sm_.state() == RunState::Paused) {
        return resume(diagnostics);
    }
    cursor_ = findFirstIncomplete();
    if (!transitionLogged(RunState::Running, "start")) {
        diagnostics = "cannot enter Running";
        return false;
    }
    pause_requested_ = false;
    stop_requested_ = false;
    return true;
}

bool MeasurementOrchestrator::pause()
{
    if (sm_.state() != RunState::Running) {
        return false;
    }
    // Флаг: дождаться текущего состояния, затем Pausing → Paused (FR-10).
    pause_requested_ = true;
    return true;
}

bool MeasurementOrchestrator::resume(std::string& diagnostics)
{
    diagnostics.clear();
    if (sm_.state() != RunState::Paused) {
        diagnostics = "resume requires Paused";
        return false;
    }
    if (!ensureHardwareReady(diagnostics)) {
        last_error_ = diagnostics;
        (void)transitionLogged(RunState::Error, diagnostics);
        return false;
    }
    cursor_ = findFirstIncomplete();
    pause_requested_ = false;
    stop_requested_ = false;
    if (!transitionLogged(RunState::Running, "resume")) {
        diagnostics = "cannot re-enter Running";
        return false;
    }
    return true;
}

bool MeasurementOrchestrator::stop()
{
    if (sm_.state() == RunState::Running) {
        stop_requested_ = true;
        return true;
    }
    if (sm_.state() == RunState::Paused) {
        // Paused→Error→Aborted: отдельный код оператора, не отказ VNA/DUT.
        constexpr const char* kReason = "OPERATOR_STOP: останов оператором из паузы";
        last_error_.clear();
        logEvent(EventLevel::Info, "OPERATOR_STOP",
                 "останов оператором из паузы");
        if (!transitionLogged(RunState::Error, kReason)) {
            return false;
        }
        dut_->set_safe_state();
        return transitionLogged(RunState::Aborted, kReason);
    }
    return false;
}

bool MeasurementOrchestrator::requestRemeasure(std::uint8_t channel,
                                               std::uint16_t att_code,
                                               const std::vector<std::uint8_t>& phases,
                                               std::string& diagnostics)
{
    diagnostics.clear();
    if (sm_.state() != RunState::Ready && sm_.state() != RunState::Paused) {
        diagnostics = "requestRemeasure: только из Ready или Paused";
        return false;
    }
    if (!store_.isOpen()) {
        diagnostics = "requestRemeasure: store не открыт";
        return false;
    }
    if (phases.empty()) {
        diagnostics = "requestRemeasure: пустой список фаз";
        return false;
    }
    // Атомарность: либо все clearCompleted успешны, либо откат сброшенных.
    std::vector<std::uint8_t> cleared_completed;
    cleared_completed.reserve(phases.size());
    auto rollbackCleared = [&]() {
        for (const auto phase : cleared_completed) {
            RawS21StateRecord rec;
            std::string restore_diag;
            if (!store_.readState(channel, att_code, phase, rec, restore_diag)) {
                continue;
            }
            if (rec.completed) {
                continue;
            }
            rec.completed = true;
            (void)store_.writeState(channel, att_code, phase, rec, restore_diag);
        }
    };
    for (const auto phase : phases) {
        const bool was_completed = store_.isCompleted(channel, att_code, phase);
        std::string diag;
        if (!store_.clearCompleted(channel, att_code, phase, diag)) {
            diagnostics = diag.empty()
                ? ("не удалось сбросить фазу " + std::to_string(phase))
                : diag;
            rollbackCleared();
            return false;
        }
        if (was_completed) {
            cleared_completed.push_back(phase);
        }
    }
    cursor_ = findFirstIncomplete();
    logEvent(EventLevel::Info, "REMEASURE",
             "cleared " + std::to_string(phases.size()) + " phase(s) ch="
                 + std::to_string(channel) + " att=" + std::to_string(att_code)
                 + "; cursor=" + std::to_string(cursor_));
    return true;
}

int MeasurementOrchestrator::settleMsFor(std::uint16_t att_code) const
{
    for (const auto& row : att_codes_.rows) {
        if (row.att_code == static_cast<int>(att_code) && row.enabled) {
            return row.settle_ms;
        }
    }
    return config_.timing.settle_ms;
}

void MeasurementOrchestrator::doSleep(int ms) const
{
    if (!sleep_enabled_ || ms <= 0) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void MeasurementOrchestrator::drainAndLogVnaErrors(const DutState* st)
{
    if (!vna_) {
        return;
    }
    try {
        const auto errs = vna_->drain_errors();
        for (const auto& text : errs) {
            if (text.empty()) {
                continue;
            }
            logEvent(EventLevel::Warning, "VNA_SCPI_ERR", text, st);
        }
    } catch (const std::exception& ex) {
        logEvent(EventLevel::Warning, "VNA_SCPI_ERR",
                 std::string("drain_errors failed: ") + ex.what(), st);
    }
}

bool MeasurementOrchestrator::confirmDutOrSettle(const DutState& expected)
{
    try {
        const DutState rb = dut_->readback();
        if (!codesMatch(rb, expected)) {
            last_error_ = "DUT readback mismatch channel="
                + std::to_string(expected.channel) + " expected att_code="
                + std::to_string(expected.att_code) + " phase_code="
                + std::to_string(expected.phase_code) + " got att_code="
                + std::to_string(rb.att_code) + " phase_code="
                + std::to_string(rb.phase_code) + " got channel="
                + std::to_string(rb.channel);
            logEvent(EventLevel::Error, "DUT_READBACK_MISMATCH", last_error_,
                     &expected);
            dut_->set_safe_state();
            (void)transitionLogged(RunState::Error, last_error_, &expected);
            return false;
        }
        // Коды подтверждены — выдержка не нужна (HW-DUT-03).
        return true;
    } catch (const std::exception& ex) {
        if (isNoReadbackException(ex.what())) {
            doSleep(settleMsFor(expected.att_code));
            return true;
        }
        last_error_ = std::string(ex.what()) + " channel="
            + std::to_string(expected.channel) + " att_code="
            + std::to_string(expected.att_code) + " phase_code="
            + std::to_string(expected.phase_code);
        logEvent(EventLevel::Error, "DUT_READBACK_FAIL", last_error_, &expected);
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Error, last_error_, &expected);
        return false;
    }
}

bool MeasurementOrchestrator::measureReference(const ScanItem& after_item)
{
    DutState ref{};
    ref.channel = after_item.state.channel;
    ref.att_code = static_cast<std::uint16_t>(config_.dut.reference.att_code);
    ref.phase_code = static_cast<std::uint8_t>(config_.dut.reference.phase_code);
    try {
        dut_->apply(ref);
    } catch (const std::exception& ex) {
        last_error_ = ex.what();
        logEvent(EventLevel::Error, "REF_FAIL", last_error_, &ref);
        return false;
    }
    if (!confirmDutOrSettle(ref)) {
        return false;
    }
    try {
        ComplexSweep sweep = measureAllFour(*vna_, sweepConfigFromRun(config_));
        drainAndLogVnaErrors(&ref);
        const auto row = scan_.referenceAttRow(after_item.state.att_code);
        if (!row) {
            return false;
        }
        std::string diag;
        // LUT/reference — только S21 (DATA-102 лёгкий путь).
        if (!store_.writeReference(ref.channel, *row, sweep.s21, diag)) {
            last_error_ = diag;
            return false;
        }
        logEvent(EventLevel::Info, "REF_OK", "reference measured", &ref);
        return true;
    } catch (const std::exception& ex) {
        drainAndLogVnaErrors(&ref);
        last_error_ = ex.what();
        logEvent(EventLevel::Error, "REF_FAIL", last_error_, &ref);
        return false;
    }
}

bool MeasurementOrchestrator::measureOneState(const ScanItem& item)
{
    const auto& st = item.state;
    if (store_.isCompleted(st.channel, st.att_code, st.phase_code)) {
        return true;
    }

    std::uint16_t attempt = 0;
    {
        RawS21StateRecord prev;
        std::string diag;
        if (store_.readState(st.channel, st.att_code, st.phase_code, prev, diag)) {
            attempt = prev.attempt;
        }
    }

    try {
        dut_->apply(st);
    } catch (const std::exception& ex) {
        // AT-08: не вызывать measure без подтверждения apply.
        last_error_ = std::string(ex.what()) + " channel=" + std::to_string(st.channel)
            + " att_code=" + std::to_string(st.att_code)
            + " phase_code=" + std::to_string(st.phase_code);
        logEvent(EventLevel::Error, "DUT_REJECT", last_error_, &st);
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Error, last_error_);
        return false;
    }

    if (!confirmDutOrSettle(st)) {
        return false;
    }

    ComplexSweep sweep;
    bool vna_error = false;
    std::string vna_msg;
    const int max_tries = 1 + kVnaRetries;
    const SweepConfig base_sweep = sweepConfigFromRun(config_);
    for (int try_i = 0; try_i < max_tries; ++try_i) {
        ++attempt;
        try {
            sweep = measureAllFour(*vna_, base_sweep);
            drainAndLogVnaErrors(&st);
            vna_error = false;
            break;
        } catch (const std::exception& ex) {
            drainAndLogVnaErrors(&st);
            vna_error = true;
            vna_msg = ex.what();
            logEvent(EventLevel::Warning, "VNA_RETRY", vna_msg, &st, static_cast<int>(attempt));
            if (try_i + 1 >= max_tries) {
                break;
            }
        }
    }

    RawS21StateRecord rec;
    rec.attempt = attempt;
    rec.temperature_c = static_cast<float>(dut_->temperature_c());
    rec.completed = true;

    if (vna_error) {
        // AT-07: помечаем invalid после исчерпания повторов.
        rec.s21.assign(frequency_axis_.size(), {0.0, 0.0});
        rec.valid.assign(frequency_axis_.size(), 0);
        rec.overload = false;
        last_error_ = vna_msg;
        logEvent(EventLevel::Error, "VNA_TIMEOUT", last_error_, &st, static_cast<int>(attempt));
        std::string diag;
        (void)store_.writeState(st.channel, st.att_code, st.phase_code, rec, diag);
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Error, last_error_);
        return false;
    }

    // LUT — только S21; полный ComplexSweep остаётся в last_sweep_ для UI.
    rec.s21 = sweep.s21;
    rec.overload = sweep.overload;

    qc::QualityInputs qi;
    qi.code_confirmed = true;
    qi.vna_timeout_or_error = false;
    qi.length_mismatch =
        (sweep.frequency_hz.size() != frequency_axis_.size()
         || sweep.s21.size() != frequency_axis_.size());
    qi.has_nan_or_inf = hasNanOrInf(sweep);
    qi.overload = sweep.overload;

    qc::QualityThresholds th;
    th.max_drift_phase_deg = config_.limits.max_drift_phase_deg;
    th.max_phase_residual_deg = config_.limits.max_phase_residual_deg;
    const bool ok = qc::evaluate_valid(qi, th);

    rec.valid.assign(frequency_axis_.size(), ok ? 1 : 0);

    std::string diag;
    if (!store_.writeState(st.channel, st.att_code, st.phase_code, rec, diag)) {
        last_error_ = diag;
        logEvent(EventLevel::Error, "STORE_FAIL", diag, &st, static_cast<int>(attempt));
        (void)transitionLogged(RunState::Error, diag);
        return false;
    }
    last_sweep_ = sweep;
    if (last_sweep_.frequency_hz.empty()) {
        last_sweep_.frequency_hz = frequency_axis_;
    }
    has_last_sweep_ = true;
    logEvent(EventLevel::Info, "STATE_OK", "measured", &st, static_cast<int>(attempt));

    if (item.need_reference) {
        if (!measureReference(item)) {
            (void)transitionLogged(RunState::Error, last_error_);
            return false;
        }
    }
    return true;
}

bool MeasurementOrchestrator::stepOnce()
{
    if (sm_.state() == RunState::Stopping) {
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Aborted, "stopped");
        return false;
    }
    if (sm_.state() == RunState::Pausing) {
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Paused, "paused");
        pause_requested_ = false;
        return false;
    }
    if (sm_.state() != RunState::Running) {
        return false;
    }

    // Stop на границе состояний — без нового измерения.
    if (stop_requested_) {
        (void)transitionLogged(RunState::Stopping, "stop");
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Aborted, "stopped");
        return false;
    }

    while (cursor_ < scan_.size()) {
        const auto& item = scan_.at(cursor_);
        if (store_.isCompleted(item.state.channel, item.state.att_code, item.state.phase_code)) {
            ++cursor_;
            continue;
        }
        break;
    }
    if (cursor_ >= scan_.size()) {
        const DutState* stand = nullptr;
        DutState snap{};
        if (has_last_sweep_ && cursor_ > 0) {
            snap = appliedStand(scan_.at(cursor_ - 1), config_);
            stand = &snap;
        }
        (void)transitionLogged(RunState::Finalizing, "all completed", stand);
        dut_->set_safe_state();
        std::string export_diag;
        if (!finalizeExports(export_diag)) {
            last_error_ = export_diag;
            logEvent(EventLevel::Error, "EXPORT_FAIL", export_diag);
            (void)transitionLogged(RunState::Error, export_diag);
            return false;
        }
        // Строка Complete в журнал до смены состояния — манифест ещё в Finalizing.
        {
            std::string text = std::string(toString(sm_.state())) + " -> "
                + toString(RunState::Complete) + ": done; пользователь: нет оператора";
            if (stand != nullptr) {
                text += "; channel=" + std::to_string(stand->channel);
                text += " att_code=" + std::to_string(stand->att_code);
                text += " phase_code=" + std::to_string(stand->phase_code);
            }
            if (dut_ != nullptr) {
                try {
                    const double temperature_c = dut_->temperature_c();
                    std::ostringstream oss;
                    oss << "; temperature_c=" << temperature_c;
                    text += oss.str();
                } catch (const std::exception&) {
                }
            }
            logEvent(EventLevel::Info, "STATE_TRANSITION", text, stand);
        }
        if (!report::writeManifest(series_, export_diag)) {
            last_error_ = export_diag;
            logEvent(EventLevel::Error, "EXPORT_FAIL", export_diag);
            (void)transitionLogged(RunState::Error, export_diag);
            return false;
        }
        if (!sm_.tryTransition(RunState::Complete)) {
            logEvent(EventLevel::Warning, "STATE_REJECT",
                     "rejected Finalizing -> Complete after manifest");
            return false;
        }
        return false;
    }

    // Пауза на границе: если флаг уже стоит до старта слота — уходим в Paused.
    // Если флаг поставят во время (другой поток) — проверим после measure.
    if (pause_requested_) {
        (void)transitionLogged(RunState::Pausing, "pause before next state");
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Paused, "paused");
        pause_requested_ = false;
        return false;
    }

    const auto& item = scan_.at(cursor_);
    if (!measureOneState(item)) {
        return false;
    }
    ++cursor_;

    if (sm_.state() != RunState::Running) {
        return false;
    }
    if (pause_requested_) {
        const DutState snap = appliedStand(scan_.at(cursor_ - 1), config_);
        (void)transitionLogged(RunState::Pausing, "pause after state", &snap);
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Paused, "paused");
        pause_requested_ = false;
        return false;
    }
    if (stop_requested_) {
        const DutState snap = appliedStand(scan_.at(cursor_ - 1), config_);
        (void)transitionLogged(RunState::Stopping, "stop after state", &snap);
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Aborted, "stopped");
        return false;
    }
    return true;
}

void MeasurementOrchestrator::runUntilDone()
{
    while (sm_.state() == RunState::Running) {
        if (!stepOnce()) {
            break;
        }
    }
    if (sm_.state() == RunState::Pausing || sm_.state() == RunState::Stopping) {
        (void)stepOnce();
    }
}

bool MeasurementOrchestrator::finalizeExports(std::string& diagnostics)
{
    diagnostics.clear();

    std::vector<cal::DirectLutEntry> direct;
    if (!report::buildDirectLutFromStore(store_, config_, direct, diagnostics)) {
        return false;
    }
    if (!report::exportDirectLut(series_.directLutPath(), direct, diagnostics)) {
        return false;
    }

    std::vector<report::InverseLutEntry> inverse;
    if (!report::buildInverseLutFromDirect(direct, config_, att_codes_, inverse, diagnostics)) {
        return false;
    }
    if (!report::exportInverseLut(series_.inverseLutPath(), inverse, diagnostics)) {
        return false;
    }

    report::RunReportInfo pdf_info;
    pdf_info.run_id = config_.run_id;
    pdf_info.completed_states = store_.completedCount();
    pdf_info.valid_direct_count = report::countValidDirect(direct);
    pdf_info.series_path = series_.root().string();
    pdf_info.max_drift_phase_deg = config_.limits.max_drift_phase_deg;
    pdf_info.max_phase_residual_deg = config_.limits.max_phase_residual_deg;
    if (!report::writeRunReportPdf(series_.reportPath(), pdf_info, diagnostics)) {
        return false;
    }

    // GAP-RAW-001: табличное сырьё т. 7.3 (до манифеста — файл попадёт в SHA-256).
    if (!report::exportRawS21Csv(series_.rawS21CsvPath(), store_, config_.run_id,
                                 series_.runEventsPath(), diagnostics)) {
        return false;
    }

    // manifest.sha256 — в stepOnce после строки Complete в журнале (ещё Finalizing).
    return true;
}

bool MeasurementOrchestrator::probeIdentify(std::string& idn_or_diagnostics)
{
    idn_or_diagnostics.clear();
    last_error_.clear();
    if (!vna_) {
        idn_or_diagnostics = "VNA pointer is null";
        last_error_ = idn_or_diagnostics;
        return false;
    }
    if (sm_.state() != RunState::Idle) {
        idn_or_diagnostics = "probeIdentify: требуется Idle";
        last_error_ = idn_or_diagnostics;
        return false;
    }
    try {
        vna_->connect();
        const auto idn = vna_->identify();
        if (!idnContainsC2220(idn)) {
            idn_or_diagnostics = "VNA IDN does not contain C2220: " + idn;
            last_error_ = idn_or_diagnostics;
            vna_->abort();
            logEvent(EventLevel::Error, "HW_PROBE_FAIL", idn_or_diagnostics);
            return false;
        }
        idn_or_diagnostics = idn;
        logEvent(EventLevel::Info, "HW_PROBE_OK", idn);
        return true;
    } catch (const std::exception& ex) {
        idn_or_diagnostics = ex.what();
        last_error_ = idn_or_diagnostics;
        vna_->abort();
        logEvent(EventLevel::Error, "HW_PROBE_FAIL", idn_or_diagnostics);
        return false;
    }
}

bool MeasurementOrchestrator::runProbeCodes(const SweepConfig& sweep,
                                            std::uint16_t att_last,
                                            std::uint8_t phase_last,
                                            std::string& diagnostics)
{
    diagnostics.clear();
    last_error_.clear();
    if (!vna_ || !dut_) {
        diagnostics = "VNA/DUT pointers are null";
        last_error_ = diagnostics;
        return false;
    }
    if (sm_.state() != RunState::Idle) {
        diagnostics = "runProbeCodes: требуется Idle";
        last_error_ = diagnostics;
        return false;
    }

    auto fail = [&](const std::string& msg) {
        diagnostics = msg;
        last_error_ = msg;
        vna_->abort();
        dut_->set_safe_state();
        logEvent(EventLevel::Error, "PROBE_CODES_FAIL", msg);
        return false;
    };

    try {
        vna_->connect();
        dut_->connect();
        const auto idn = vna_->identify();
        if (!idnContainsC2220(idn)) {
            return fail("VNA IDN does not contain C2220: " + idn);
        }
        vna_->configure(sweep);
        drainAndLogVnaErrors();

        const std::uint16_t atts[2] = {0, att_last};
        const std::uint8_t phases[2] = {0, phase_last};
        const int n_att = (att_last == 0) ? 1 : 2;
        const int n_ph = (phase_last == 0) ? 1 : 2;

        for (int ai = 0; ai < n_att; ++ai) {
            for (int pi = 0; pi < n_ph; ++pi) {
                DutState st{};
                st.channel = 1;
                st.att_code = atts[ai];
                st.phase_code = phases[pi];
                dut_->apply(st);
                if (!confirmDutOrSettle(st)) {
                    return fail(last_error_.empty() ? "DUT confirm/settle failed" : last_error_);
                }
                try {
                    (void)vna_->measure_s21();
                    drainAndLogVnaErrors(&st);
                } catch (const std::exception&) {
                    drainAndLogVnaErrors(&st);
                    throw;
                }
                logEvent(EventLevel::Info, "PROBE_CODE_OK",
                         "ch=1 att=" + std::to_string(st.att_code)
                             + " phase=" + std::to_string(st.phase_code),
                         &st);
            }
        }
        dut_->set_safe_state();
        diagnostics = "пробные коды: ch=1 att=0.." + std::to_string(att_last)
            + " phase=0.." + std::to_string(phase_last);
        logEvent(EventLevel::Info, "PROBE_CODES_OK", diagnostics);
        return true;
    } catch (const std::exception& ex) {
        return fail(ex.what());
    }
}

}  // namespace afar
