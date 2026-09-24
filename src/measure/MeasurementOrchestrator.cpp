#include "MeasurementOrchestrator.h"

#include "Manifest.h"
#include "ParquetExport.h"
#include "QualityGates.h"
#include "RunReportPdf.h"

#include <chrono>
#include <cmath>
#include <thread>

namespace afar {
namespace {

bool idnContainsSupportedPlanar(const std::string& idn)
{
    return idn.find("C1220") != std::string::npos || idn.find("C2220") != std::string::npos;
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
                                       const std::string& message,
                                       const DutState* st,
                                       std::optional<int> attempt)
{
    if (!log_.isOpen()) {
        return;
    }
    RunEvent ev;
    ev.time_utc = RunEventLog::nowUtcIso8601();
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
    ev.message = message;
    std::string diag;
    (void)log_.append(ev, diag);
}

bool MeasurementOrchestrator::transitionLogged(RunState target, const std::string& reason)
{
    const auto from = sm_.state();
    if (!sm_.tryTransition(target)) {
        logEvent(EventLevel::Warning, "STATE_REJECT",
                 std::string("rejected ") + toString(from) + " -> " + toString(target)
                     + ": " + reason);
        return false;
    }
    logEvent(EventLevel::Info, "STATE_TRANSITION",
             std::string(toString(from)) + " -> " + toString(target) + ": " + reason);
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
        if (!idnContainsSupportedPlanar(idn)) {
            diagnostics = "VNA IDN does not contain C1220/C2220: " + idn;
            return false;
        }
        SweepConfig sweep{};
        sweep.f_start_hz = config_.vna.f_start_hz;
        sweep.f_stop_hz = config_.vna.f_stop_hz;
        sweep.points = static_cast<std::uint32_t>(config_.vna.points);
        sweep.power_dbm = config_.vna.power_dbm;
        sweep.ifbw_hz = static_cast<std::uint32_t>(config_.vna.ifbw_hz);
        sweep.averages = static_cast<std::uint16_t>(config_.vna.averages);
        sweep.parameter = SParameter::S21;
        vna_->configure(sweep);
        const auto errors = vna_->drain_errors();
        if (!errors.empty()) {
            diagnostics = "VNA configure error: " + errors.front();
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        diagnostics = ex.what();
        return false;
    }
}

bool MeasurementOrchestrator::prepare(const std::filesystem::path& data_root,
                                      const std::filesystem::path& run_config_src,
                                      const std::filesystem::path& attenuator_csv_src,
                                      std::string& diagnostics)
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
    if (!RawS21Store::create(series_.rawS21Path(), scan_.channels(), scan_.enabledAttCodes(),
                             scan_.phaseCodes(), frequency_axis_, store_, store_diag)) {
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
        if (!transitionLogged(RunState::Error, "stop from paused")) {
            return false;
        }
        dut_->set_safe_state();
        return transitionLogged(RunState::Aborted, "stop from paused");
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

bool MeasurementOrchestrator::measureReference(const ScanItem& after_item)
{
    DutState ref{};
    ref.channel = after_item.state.channel;
    ref.att_code = static_cast<std::uint16_t>(config_.dut.reference.att_code);
    ref.phase_code = static_cast<std::uint8_t>(config_.dut.reference.phase_code);
    try {
        dut_->apply(ref);
        doSleep(settleMsFor(ref.att_code));
        ComplexSweep sweep = vna_->measure_s21();
        const auto row = scan_.referenceAttRow(after_item.state.att_code);
        if (!row) {
            return false;
        }
        std::string diag;
        if (!store_.writeReference(ref.channel, *row, sweep.s21, diag)) {
            last_error_ = diag;
            return false;
        }
        logEvent(EventLevel::Info, "REF_OK", "reference measured", &ref);
        return true;
    } catch (const std::exception& ex) {
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

    doSleep(settleMsFor(st.att_code));

    ComplexSweep sweep;
    bool vna_error = false;
    std::string vna_msg;
    const int max_tries = 1 + kVnaRetries;
    for (int try_i = 0; try_i < max_tries; ++try_i) {
        ++attempt;
        try {
            sweep = vna_->measure_s21();
            const auto errors = vna_->drain_errors();
            if (!errors.empty()) {
                throw std::runtime_error("VNA measurement error: " + errors.front());
            }
            vna_error = false;
            break;
        } catch (const std::exception& ex) {
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
        (void)transitionLogged(RunState::Finalizing, "all completed");
        dut_->set_safe_state();
        std::string export_diag;
        if (!finalizeExports(export_diag)) {
            last_error_ = export_diag;
            logEvent(EventLevel::Error, "EXPORT_FAIL", export_diag);
            (void)transitionLogged(RunState::Error, export_diag);
            return false;
        }
        (void)transitionLogged(RunState::Complete, "done");
        // Обновить манифест после записи STATE_TRANSITION Complete в run-events.
        if (!report::writeManifest(series_, export_diag)) {
            last_error_ = export_diag;
            logEvent(EventLevel::Error, "EXPORT_FAIL", export_diag);
            (void)transitionLogged(RunState::Error, export_diag);
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
        (void)transitionLogged(RunState::Pausing, "pause after state");
        dut_->set_safe_state();
        (void)transitionLogged(RunState::Paused, "paused");
        pause_requested_ = false;
        return false;
    }
    if (stop_requested_) {
        (void)transitionLogged(RunState::Stopping, "stop after state");
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
    if (!report::writeRunReportPdf(series_.reportPath(), pdf_info, diagnostics)) {
        return false;
    }

    if (!report::writeManifest(series_, diagnostics)) {
        return false;
    }
    return true;
}

}  // namespace afar
