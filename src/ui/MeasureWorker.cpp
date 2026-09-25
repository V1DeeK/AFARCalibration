#include "MeasureWorker.h"

#include "AttenuatorCodes.h"
#include "C2220Vna.h"
#include "ParquetExport.h"
#include "PhaseMath.h"
#include "RawS21Store.h"
#include "RunConfig.h"
#include "RunEventLog.h"
#include "ScpiComTransport.h"
#include "ScpiSocketTransport.h"

#include "afar/ScpiIdn.h"
#include "StubDutController.h"

#include <QTimer>

#include <cmath>
#include <complex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct DiskSnippet {
    qint64 count{-1};
    QString fragment;
};

QString joinLines(const std::vector<std::string>& lines)
{
    QString out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            out += QLatin1Char('\n');
        }
        out += QString::fromUtf8(lines[i].data(), static_cast<int>(lines[i].size()));
    }
    return out;
}

DiskSnippet readLutSnippet(const std::filesystem::path& path)
{
    DiskSnippet out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return out;
    }
    char magic[sizeof(afar::report::kAfarPqMagic)]{};
    in.read(magic, static_cast<std::streamsize>(sizeof(magic)));
    if (!in || std::memcmp(magic, afar::report::kAfarPqMagic, sizeof(magic)) != 0) {
        out.fragment = QStringLiteral("файл открыт, заголовок AFARPQ не совпал");
        return out;
    }
    std::string line;
    std::vector<std::string> head;
    qint64 valid = 0;
    bool header = true;
    bool any = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        any = true;
        if (head.size() < 4) {
            head.push_back(line);
        }
        if (header) {
            header = false;
            continue;
        }
        const auto tab = line.rfind('\t');
        const auto field = (tab == std::string::npos) ? line : line.substr(tab + 1);
        if (field == "1") {
            ++valid;
        }
    }
    out.fragment = any ? joinLines(head) : QStringLiteral("(после AFARPQ строк нет)");
    out.count = valid;
    return out;
}

DiskSnippet readReportSnippet(const std::filesystem::path& path)
{
    DiskSnippet out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return out;
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::string bytes = oss.str();
    if (bytes.size() < 5 || bytes.compare(0, 5, "%PDF-") != 0) {
        out.fragment = QStringLiteral("файл открыт, заголовок PDF не совпал");
        return out;
    }
    const std::string key = "valid_direct_count:";
    const auto pos = bytes.find(key);
    if (pos != std::string::npos) {
        std::size_t i = pos + key.size();
        while (i < bytes.size() && (bytes[i] == ' ' || bytes[i] == '\t')) {
            ++i;
        }
        std::size_t end = i;
        while (end < bytes.size() && bytes[end] >= '0' && bytes[end] <= '9') {
            ++end;
        }
        if (end > i) {
            try {
                out.count = static_cast<qint64>(std::stoll(bytes.substr(i, end - i)));
            } catch (...) {
                out.count = -1;
            }
        }
    }
    std::vector<std::string> head;
    std::size_t cursor = 0;
    while (head.size() < 6 && cursor < bytes.size()) {
        const auto open = bytes.find('(', cursor);
        if (open == std::string::npos) {
            break;
        }
        const auto close = bytes.find(')', open + 1);
        if (close == std::string::npos) {
            break;
        }
        const auto inner = bytes.substr(open + 1, close - open - 1);
        const bool reportLine = inner.find("AFAR") != std::string::npos
            || inner.find("run_id") != std::string::npos
            || inner.find("valid_direct_count") != std::string::npos
            || inner.find("completed_states") != std::string::npos
            || inner.find("software_version") != std::string::npos
            || inner.find("series_path") != std::string::npos;
        if (!inner.empty() && reportLine) {
            head.push_back(inner);
        }
        cursor = close + 1;
    }
    out.fragment = head.empty() ? QStringLiteral("(в PDF нет текстовых строк протокола)")
                                : joinLines(head);
    return out;
}

DiskSnippet readManifestSnippet(const std::filesystem::path& path)
{
    DiskSnippet out;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return out;
    }
    std::string line;
    std::vector<std::string> head;
    qint64 lines = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        ++lines;
        if (head.size() < 6) {
            head.push_back(line);
        }
    }
    out.count = lines;
    out.fragment = head.empty() ? QStringLiteral("(манифест пуст)") : joinLines(head);
    return out;
}

enum CellStatus : int {
    CellWaiting = 0,
    CellMeasuring = 1,
    CellDone = 2,
    CellRetry = 3,
    CellError = 4,
    /// Фаза отсутствует в оси store (урезанный phase_codes).
    CellOutOfAxis = -1,
};

}  // namespace

MeasureWorker::MeasureWorker(QObject* parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(10);
    connect(m_timer, &QTimer::timeout, this, &MeasureWorker::onTick);
    rebuildVna();
    m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
}

MeasureWorker::~MeasureWorker() = default;

void MeasureWorker::rebuildVna()
{
    m_c2220.reset();
    m_socket.reset();
    m_com.reset();
    m_simVna.reset();
    m_vna = nullptr;
    m_lastIdn.clear();

    C2220Vna::Profile profile;
    profile.allow_direct_access = m_allowDirect;
    profile.connect_timeout_ms = static_cast<std::uint32_t>(
        m_connectTimeoutMs > 0 ? m_connectTimeoutMs : 3000);
    profile.sweep_timeout_ms = static_cast<std::uint32_t>(
        m_sweepTimeoutMs > 0 ? m_sweepTimeoutMs : 30000);
    profile.measure_retries = m_measureRetries >= 0 ? m_measureRetries : 2;

    if (m_backend == BackendSocket) {
        m_socket = std::make_unique<ScpiSocketTransport>(m_host.toStdString(),
                                                          static_cast<std::uint16_t>(m_port));
        m_c2220 = std::make_unique<C2220Vna>(*m_socket, profile);
        m_vna = m_c2220.get();
    } else if (m_backend == BackendCom) {
        m_com = std::make_unique<ScpiComTransport>(m_comPort.toStdString());
        m_c2220 = std::make_unique<C2220Vna>(*m_com, profile);
        m_vna = m_c2220.get();
    } else {
        m_simVna = std::make_unique<VnaSimulator>();
        m_vna = m_simVna.get();
    }
}

IVna* MeasureWorker::activeVna()
{
    return m_vna;
}

void MeasureWorker::emitPendingScpiErrors()
{
    if (!m_orch) {
        return;
    }
    const auto errs = m_orch->takeLastScpiErrors();
    if (errs.empty()) {
        return;
    }
    QStringList lines;
    lines.reserve(static_cast<int>(errs.size()));
    for (const auto& e : errs) {
        lines << QString::fromStdString(e);
    }
    emit scpiErrorsReceived(lines);
}

void MeasureWorker::configureVna(int backend,
                                 const QString& host,
                                 int port,
                                 const QString& comPort,
                                 bool allowDirectAccess,
                                 int connectTimeoutMs,
                                 int sweepTimeoutMs,
                                 int measureRetries)
{
    m_timer->stop();
    m_orch.reset();
    m_backend = backend;
    m_host = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    m_port = (port > 0 && port < 65536) ? port : 5025;
    m_comPort = comPort.trimmed().isEmpty() ? QStringLiteral("COM3") : comPort.trimmed();
    m_allowDirect = allowDirectAccess;
    m_connectTimeoutMs = connectTimeoutMs > 0 ? connectTimeoutMs : 3000;
    m_sweepTimeoutMs = sweepTimeoutMs > 0 ? sweepTimeoutMs : 30000;
    m_measureRetries = measureRetries >= 0 ? measureRetries : 2;
    rebuildVna();
    m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    emitConnection();
    emit diagnostic(QString());
}

void MeasureWorker::configureController(int backend,
                                        const QString& host,
                                        int port,
                                        const QString& comPort)
{
    // Серия / оркестратор остаются на DutSimulator — Stub только для диагностики т. 14.
    if (backend == CtrlCombat) {
        emit diagnostic(QStringLiteral(
            "Боевой COM/TCP недоступен: протокол контроллера не передан (т. 14 ТЗ)"));
        m_ctrlBackend = CtrlStub;
    } else if (backend == CtrlStub) {
        m_ctrlBackend = CtrlStub;
    } else {
        m_ctrlBackend = CtrlSimulator;
    }
    m_dutHost = host.trimmed().isEmpty() ? QStringLiteral("192.168.0.10") : host.trimmed();
    m_dutPort = (port > 0 && port < 65536) ? port : 4001;
    m_dutComPort = comPort.trimmed().isEmpty() ? QStringLiteral("COM4") : comPort.trimmed();

    if (m_ctrlBackend == CtrlStub) {
        try {
            StubDutController stub;
            stub.connect();
            emit diagnostic(QStringLiteral("StubDutController: неожиданный успех connect"));
        } catch (const std::exception& ex) {
            emit diagnostic(QString::fromUtf8(ex.what()));
        } catch (...) {
            emit diagnostic(QStringLiteral("протокол не передан (т. 14 ТЗ); COM/TCP не открываются"));
        }
    } else {
        emit diagnostic(QString());
    }
    emitConnection();
}

void MeasureWorker::probeVna()
{
    if (!m_vna || !m_orch) {
        emit probeFinished(false, QStringLiteral("VNA не сконфигурирован"));
        return;
    }
    using afar::RunState;
    const auto st = m_orch->state();
    if (st == RunState::Running || st == RunState::Pausing || st == RunState::Paused
        || st == RunState::Stopping || st == RunState::Connecting || st == RunState::SelfTest
        || st == RunState::Finalizing || st == RunState::Ready) {
        emit probeFinished(false,
                           QStringLiteral("Проверка связи только из Idle (цикл не активен)"));
        return;
    }
    if (st != RunState::Idle) {
        // Complete / Error / Aborted — новый оркестратор в Idle.
        m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    }
    std::string idn;
    const bool ok = m_orch->probeIdentify(idn);
    if (ok) {
        m_lastIdn = QString::fromStdString(idn);
    } else {
        m_lastIdn.clear();
    }
    emitConnection();
    emitState();
    emit probeFinished(ok, QString::fromStdString(idn));
    if (ok && m_c2220) {
        try {
            SweepConfig instrument{};
            const ComplexSweep sweep = m_c2220->read_current_trace(&instrument);
            emit instrumentTraceDetected(static_cast<double>(instrument.f_start_hz),
                                         static_cast<double>(instrument.f_stop_hz),
                                         static_cast<int>(instrument.points),
                                         static_cast<int>(instrument.ifbw_hz),
                                         instrument.power_dbm,
                                         static_cast<int>(instrument.s_parameter));
            emitSweepPreview(sweep);
            emit diagnostic(
                QStringLiteral("Живая трасса S2VNA получена: %1 точек, %2…%3 ГГц")
                    .arg(instrument.points)
                    .arg(static_cast<double>(instrument.f_start_hz) / 1e9, 0, 'f', 6)
                    .arg(static_cast<double>(instrument.f_stop_hz) / 1e9, 0, 'f', 6));
        } catch (const std::exception& ex) {
            emit diagnostic(QStringLiteral("Связь установлена, но трасса S2VNA не прочитана: %1")
                                .arg(QString::fromUtf8(ex.what())));
        }
    } else if (ok) {
        emit diagnostic(QStringLiteral("IDN: %1").arg(QString::fromStdString(idn)));
    } else {
        emit diagnostic(QString::fromStdString(idn));
    }
    emitPendingScpiErrors();
}

void MeasureWorker::simulateScpiError()
{
    if (!m_simVna) {
        emit diagnostic(QStringLiteral("Симуляция SCPI-ошибки только в режиме «Имитатор»"));
        return;
    }
    // Формат SYST:ERR? из контракта: <код>, <текст>. Не новая мнемоника.
    m_simVna->push_instrument_error("-100,\"Command error\"");
    const auto errs = m_simVna->drain_errors();
    QStringList lines;
    for (const auto& e : errs) {
        lines << QString::fromStdString(e);
    }
    if (!lines.isEmpty()) {
        emit scpiErrorsReceived(lines);
    }
}

void MeasureWorker::runProbeCodes(double fStartHz,
                                  double fStopHz,
                                  int points,
                                  int ifbwHz,
                                  double powerDbm,
                                  int averages)
{
    if (!m_vna || !m_orch) {
        emit probeCodesFinished(false, QStringLiteral("VNA не сконфигурирован"));
        return;
    }
    using afar::RunState;
    const auto st = m_orch->state();
    if (st == RunState::Running || st == RunState::Pausing || st == RunState::Paused
        || st == RunState::Stopping || st == RunState::Connecting || st == RunState::SelfTest
        || st == RunState::Finalizing || st == RunState::Ready) {
        emit probeCodesFinished(
            false, QStringLiteral("Пробные коды только из Idle (серия не подготовлена)"));
        return;
    }
    if (st != RunState::Idle) {
        m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    }
    // На живом VNA (S2VNA/COM) settle через doSleep обязателен: DutSimulator
    // без readback идёт в confirmDutOrSettle → doSleep. Ускоряем только имитатор.
    if (m_backend == BackendSimulator) {
        m_orch->setSleepEnabled(false);
    }

    SweepConfig sweep{};
    sweep.f_start_hz = static_cast<std::uint64_t>(fStartHz + 0.5);
    sweep.f_stop_hz = static_cast<std::uint64_t>(fStopHz + 0.5);
    sweep.points = static_cast<std::uint32_t>(points > 1 ? points : 11);
    sweep.ifbw_hz = static_cast<std::uint32_t>(ifbwHz > 0 ? ifbwHz : 1000);
    sweep.power_dbm = powerDbm;
    sweep.averages = static_cast<std::uint16_t>(averages > 0 ? averages : 1);

    // Короткий набор: att 0 и последний «типичный» enabled (1), фазы 0 и 63.
    constexpr std::uint16_t kAttLast = 1;
    constexpr std::uint8_t kPhaseLast = 63;
    std::string diag;
    const bool ok = m_orch->runProbeCodes(sweep, kAttLast, kPhaseLast, diag);
    emitConnection();
    emitState();
    emit probeCodesFinished(ok, QString::fromStdString(diag));
    emitPendingScpiErrors();
    emit diagnostic(ok ? QStringLiteral("Пробные коды: OK")
                       : QString::fromStdString(diag));
}

void MeasureWorker::measureNow(double fStartHz,
                               double fStopHz,
                               int points,
                               int ifbwHz,
                               double powerDbm,
                               int averages)
{
    if (!m_vna || !m_orch) {
        emit measureNowFinished(false, QStringLiteral("VNA не сконфигурирован"));
        return;
    }
    if (!std::isfinite(fStartHz) || !std::isfinite(fStopHz) || fStartHz >= fStopHz) {
        emit measureNowFinished(
            false, QStringLiteral("Некорректный диапазон: f нач. должна быть меньше f кон.; "
                                  "предыдущий график сохранён"));
        return;
    }
    using afar::RunState;
    const auto st = m_orch->state();
    if (st == RunState::Running || st == RunState::Pausing || st == RunState::Stopping
        || st == RunState::Finalizing || st == RunState::Connecting
        || st == RunState::SelfTest) {
        emit measureNowFinished(
            false, QStringLiteral("«Измерить сейчас» недоступно во время серии"));
        return;
    }
    if (st != RunState::Idle && st != RunState::Ready) {
        // Complete / Error / Aborted / Paused — новый оркестратор в Idle.
        m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    }

    SweepConfig sweep{};
    sweep.f_start_hz = static_cast<std::uint64_t>(fStartHz + 0.5);
    sweep.f_stop_hz = static_cast<std::uint64_t>(fStopHz + 0.5);
    sweep.points = static_cast<std::uint32_t>(points > 1 ? points : 11);
    sweep.ifbw_hz = static_cast<std::uint32_t>(ifbwHz > 0 ? ifbwHz : 1000);
    sweep.power_dbm = powerDbm;
    sweep.averages = static_cast<std::uint16_t>(averages > 0 ? averages : 1);

    std::string diag;
    const bool ok = m_orch->measurePreview(sweep, diag);
    emitConnection();
    emitState();
    if (ok) {
        m_sweepThrottleArmed = false;  // сразу обновить графики
        maybeEmitSweepPreview();
        emit measureNowFinished(true, QString::fromStdString(diag));
        emit diagnostic(QStringLiteral("Измерить сейчас: OK"));
    } else {
        emit measureNowFinished(false, QString::fromStdString(diag));
        emit diagnostic(QString::fromStdString(diag));
    }
}

void MeasureWorker::calibrateTwoPort(int step)
{
    if (!m_vna || !m_orch) {
        emit calibrateTwoPortFinished(false, step, QStringLiteral("VNA не сконфигурирован"));
        return;
    }
    using afar::RunState;
    const auto st = m_orch->state();
    if (st == RunState::Running || st == RunState::Pausing || st == RunState::Stopping
        || st == RunState::Finalizing || st == RunState::Connecting
        || st == RunState::SelfTest) {
        emit calibrateTwoPortFinished(
            false, step, QStringLiteral("Калибровка недоступна во время серии"));
        return;
    }
    if (st != RunState::Idle && st != RunState::Ready) {
        m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    }

    constexpr int kMax =
        static_cast<int>(TwoPortCalibrationStep::Apply);
    if (step < 0 || step > kMax) {
        emit calibrateTwoPortFinished(false, step, QStringLiteral("Неизвестный шаг SOLT"));
        return;
    }
    std::string diag;
    const bool ok = m_orch->calibrateTwoPortStep(
        static_cast<TwoPortCalibrationStep>(step), diag);
    emitConnection();
    emitState();
    emit calibrateTwoPortFinished(ok, step, QString::fromStdString(diag));
    emit diagnostic(ok ? QStringLiteral("SOLT шаг %1: OK").arg(step)
                       : QString::fromStdString(diag));
}

void MeasureWorker::calibrateOnePort(int step, int port)
{
    if (!m_vna || !m_orch) {
        emit calibrateOnePortFinished(false, step, QStringLiteral("VNA не сконфигурирован"));
        return;
    }
    using afar::RunState;
    const auto st = m_orch->state();
    if (st == RunState::Running || st == RunState::Pausing || st == RunState::Stopping
        || st == RunState::Finalizing || st == RunState::Connecting
        || st == RunState::SelfTest) {
        emit calibrateOnePortFinished(
            false, step, QStringLiteral("Калибровка недоступна во время серии"));
        return;
    }
    if (st != RunState::Idle && st != RunState::Ready) {
        m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    }

    constexpr int kMax = static_cast<int>(OnePortCalibrationStep::Apply);
    if (step < 0 || step > kMax) {
        emit calibrateOnePortFinished(false, step, QStringLiteral("Неизвестный шаг OSL"));
        return;
    }
    if (port != 1 && port != 2) {
        emit calibrateOnePortFinished(false, step, QStringLiteral("Порт должен быть 1 или 2"));
        return;
    }
    std::string diag;
    const bool ok = m_orch->calibrateOnePortStep(
        static_cast<OnePortCalibrationStep>(step), port, diag);
    emitConnection();
    emitState();
    emit calibrateOnePortFinished(ok, step, QString::fromStdString(diag));
    emit diagnostic(ok ? QStringLiteral("OSL порт %1 шаг %2: OK").arg(port).arg(step)
                       : QString::fromStdString(diag));
}

QString MeasureWorker::stateToRussian(afar::RunState state)
{
    using afar::RunState;
    switch (state) {
    case RunState::Idle:
        return QStringLiteral("простой");
    case RunState::Connecting:
        return QStringLiteral("подключение");
    case RunState::SelfTest:
        return QStringLiteral("самопроверка");
    case RunState::Ready:
        return QStringLiteral("готово к старту");
    case RunState::Running:
        return QStringLiteral("измерение");
    case RunState::Pausing:
        return QStringLiteral("пауза…");
    case RunState::Paused:
        return QStringLiteral("на паузе");
    case RunState::Stopping:
        return QStringLiteral("остановка…");
    case RunState::Aborted:
        return QStringLiteral("прервано");
    case RunState::Finalizing:
        return QStringLiteral("завершение");
    case RunState::Complete:
        return QStringLiteral("завершено");
    case RunState::Error:
        return QStringLiteral("ошибка");
    case RunState::Recovery:
        return QStringLiteral("восстановление");
    }
    return QStringLiteral("неизвестно");
}

QString MeasureWorker::stateColor(afar::RunState state)
{
    using afar::RunState;
    switch (state) {
    case RunState::Ready:
    case RunState::Complete:
        return QStringLiteral("#0a7a2f");
    case RunState::Running:
    case RunState::Connecting:
    case RunState::SelfTest:
    case RunState::Finalizing:
        return QStringLiteral("#0b5cab");
    case RunState::Paused:
    case RunState::Pausing:
        return QStringLiteral("#9a6700");
    case RunState::Error:
    case RunState::Aborted:
        return QStringLiteral("#8a1f11");
    default:
        return QStringLiteral("#333333");
    }
}

QString MeasureWorker::formatEtaSeconds(qint64 totalSec)
{
    if (totalSec < 0) {
        totalSec = 0;
    }
    const qint64 h = totalSec / 3600;
    const qint64 m = (totalSec % 3600) / 60;
    const qint64 s = totalSec % 60;
    if (h > 0) {
        return QStringLiteral("ETA: ~%1 ч %2 мин %3 с").arg(h).arg(m).arg(s);
    }
    if (m > 0) {
        return QStringLiteral("ETA: ~%1 мин %2 с").arg(m).arg(s);
    }
    return QStringLiteral("ETA: ~%1 с").arg(s);
}

void MeasureWorker::resetEta()
{
    m_etaActive = false;
    m_etaBaseCompleted = -1;
}

void MeasureWorker::updateEta(qint64 completed, qint64 total)
{
    if (!m_orch) {
        return;
    }
    const auto st = m_orch->state();
    if (st == afar::RunState::Complete || (total > 0 && completed >= total)) {
        emit etaChanged(QStringLiteral("ETA: готово"));
        resetEta();
        return;
    }
    if (st != afar::RunState::Running) {
        if (!m_etaActive) {
            emit etaChanged(QStringLiteral("ETA: —"));
        }
        return;
    }
    if (!m_etaActive) {
        m_etaTimer.start();
        m_etaBaseCompleted = completed;
        m_etaActive = true;
        emit etaChanged(QStringLiteral("ETA: расчёт…"));
        return;
    }
    if (m_etaBaseCompleted < 0) {
        m_etaBaseCompleted = completed;
    }
    const qint64 delta = completed - m_etaBaseCompleted;
    const qint64 ms = m_etaTimer.elapsed();
    if (delta <= 0 || ms < 200) {
        emit etaChanged(QStringLiteral("ETA: расчёт…"));
        return;
    }
    const double rate = static_cast<double>(delta) / (static_cast<double>(ms) / 1000.0);
    if (!(rate > 0.0)) {
        emit etaChanged(QStringLiteral("ETA: расчёт…"));
        return;
    }
    const qint64 remaining = total - completed;
    const qint64 etaSec = static_cast<qint64>(std::llround(static_cast<double>(remaining) / rate));
    emit etaChanged(formatEtaSeconds(etaSec));
}

void MeasureWorker::emitConnection()
{
    QString model = QStringLiteral("PLANAR C2220");
    QString serial;
    QString firmware;
    QString address = QStringLiteral("не задан");
    QString iface = QStringLiteral("DutSimulator");
    double temp = 0.0;
    bool tempOk = false;
    bool vnaOk = false;
    bool dutOk = false;
    if (m_ctrlBackend == CtrlStub) {
        iface = QStringLiteral("Stub");
        dutOk = false;
    } else if (m_dut.connected()) {
        temp = m_dut.temperature_c();
        tempOk = true;
        iface = QStringLiteral("DutSimulator");
        dutOk = true;
    } else {
        iface = QStringLiteral("DutSimulator");
        dutOk = false;
    }
    if (m_simVna) {
        address = QStringLiteral("VnaSimulator");
        model = QStringLiteral("PLANAR C2220 (SIM)");
        vnaOk = m_simVna->connected();
    } else if (m_c2220) {
        vnaOk = m_c2220->connected();
        model = QStringLiteral("PLANAR C2220");
        if (m_backend == BackendSocket) {
            address = QStringLiteral("%1:%2").arg(m_host).arg(m_port);
        } else {
            address = m_comPort;
        }
    }
    if (!m_lastIdn.isEmpty()) {
        const auto fields = parse_scpi_idn(m_lastIdn.toStdString());
        if (!fields.model.empty()) {
            model = QString::fromStdString(fields.model);
        }
        serial = QString::fromStdString(fields.serial);
        firmware = QString::fromStdString(fields.firmware);
    } else if (m_c2220 && !m_c2220->last_idn().raw.empty()) {
        const auto& fields = m_c2220->last_idn();
        if (!fields.model.empty()) {
            model = QString::fromStdString(fields.model);
        }
        serial = QString::fromStdString(fields.serial);
        firmware = QString::fromStdString(fields.firmware);
    }
    emit connectionChanged(model, address, vnaOk, iface, dutOk, temp, tempOk, serial, firmware);
}

void MeasureWorker::emitState()
{
    if (!m_orch) {
        return;
    }
    const auto st = m_orch->state();
    emit stateChanged(static_cast<int>(st), stateToRussian(st), stateColor(st));
    if (!m_orch->lastError().empty()) {
        emit diagnostic(QString::fromStdString(m_orch->lastError()));
    }
}

void MeasureWorker::emitProgress()
{
    if (!m_orch || !m_orch->store().isOpen()) {
        return;
    }
    const qint64 total = static_cast<qint64>(m_orch->scanOrder().size());
    const qint64 done = static_cast<qint64>(m_orch->store().completedCount());
    int ch = 0;
    int att = 0;
    int ph = 0;
    const auto cursor = m_orch->cursor();
    if (total > 0 && cursor < static_cast<std::size_t>(total)) {
        const auto& item = m_orch->scanOrder().at(cursor);
        ch = item.state.channel;
        att = item.state.att_code;
        ph = item.state.phase_code;
    }
    emit progressChanged(done, total, ch, att, ph);
    updateEta(done, total);
}

void MeasureWorker::maybeEmitSweepPreview()
{
    if (!m_orch || !m_orch->hasLastMeasuredSweep()) {
        return;
    }
    constexpr qint64 kMinIntervalMs = 200;  // NFR-02: ≤ 5 Гц
    if (m_sweepThrottleArmed && m_sweepThrottle.elapsed() < kMinIntervalMs) {
        return;
    }
    m_sweepThrottle.restart();
    m_sweepThrottleArmed = true;

    emitSweepPreview(m_orch->lastMeasuredSweep());
}

void MeasureWorker::emitSweepPreview(const ComplexSweep& sweep)
{
    const auto fillMagPhase = [](const std::vector<std::complex<double>>& z,
                                 QVector<double>& magDb,
                                 QVector<double>& phaseDeg) {
        const auto unwrap = afar::cal::unwrap_phase_deg(z);
        const int n = static_cast<int>(z.size());
        magDb.resize(n);
        phaseDeg.resize(n);
        for (int i = 0; i < n; ++i) {
            magDb[i] = afar::cal::magnitude_db(z[static_cast<std::size_t>(i)]);
            phaseDeg[i] = i < static_cast<int>(unwrap.size())
                              ? unwrap[static_cast<std::size_t>(i)]
                              : 0.0;
        }
    };

    QVector<double> freqGhz;
    const int nFreq = static_cast<int>(sweep.frequency_hz.size());
    const int nS21 = static_cast<int>(sweep.s21.size());
    const int n = nFreq > 0 ? nFreq : nS21;
    freqGhz.reserve(n);
    for (int i = 0; i < n; ++i) {
        double fGhz = 0.0;
        if (i < nFreq) {
            fGhz = static_cast<double>(sweep.frequency_hz[static_cast<std::size_t>(i)]) / 1e9;
        }
        freqGhz.push_back(fGhz);
    }

    QVector<double> s11mag, s11ph, s21mag, s21ph, s12mag, s12ph, s22mag, s22ph;
    fillMagPhase(sweep.s11, s11mag, s11ph);
    fillMagPhase(sweep.s21, s21mag, s21ph);
    fillMagPhase(sweep.s12, s12mag, s12ph);
    fillMagPhase(sweep.s22, s22mag, s22ph);

    // Обратная совместимость: S21-only preview для текущего UI.
    emit sweepPreview(freqGhz, s21mag, s21ph);
    emit sparamsPreview(freqGhz, s11mag, s11ph, s21mag, s21ph, s12mag, s12ph, s22mag, s22ph);
}

void MeasureWorker::emitArtifactPreviews()
{
    if (!m_orch || m_orch->series().root().empty() || m_artifactPreviewSent) {
        return;
    }
    if (m_orch->state() != afar::RunState::Complete) {
        return;
    }
    const auto& s = m_orch->series();
    const auto direct = readLutSnippet(s.directLutPath());
    const auto inverse = readLutSnippet(s.inverseLutPath());
    const auto report = readReportSnippet(s.reportPath());
    const auto manifest = readManifestSnippet(s.manifestPath());

    qint64 directTotal = -1;
    qint64 inverseTotal = -1;
    bool directFlat = false;
    bool inverseFlat = false;
    QVector<double> lutFreq;
    QVector<double> lutMag;
    QVector<double> lutPhaseErr;
    int lutCh = 0;
    int lutAtt = 0;
    int lutPh = 0;

    {
        std::vector<afar::cal::DirectLutEntry> rows;
        std::string diag;
        if (afar::report::readDirectLut(s.directLutPath(), rows, diag)) {
            directTotal = static_cast<qint64>(rows.size());
            int checked = 0;
            int flatish = 0;
            for (const auto& e : rows) {
                if (!e.valid) {
                    continue;
                }
                ++checked;
                if (std::fabs(e.mag_db) < 0.05 || std::fabs(e.s21_re - 1.0) < 0.05) {
                    ++flatish;
                }
                if (checked >= 32) {
                    break;
                }
            }
            directFlat = checked > 0 && flatish * 2 >= checked;

            bool keyFound = false;
            std::uint8_t keyCh = 0;
            std::uint16_t keyAtt = 0;
            std::uint8_t keyPh = 0;
            for (const auto& e : rows) {
                if (!e.valid) {
                    continue;
                }
                if (!keyFound) {
                    keyCh = e.channel;
                    keyAtt = e.att_code;
                    keyPh = e.phase_code;
                    keyFound = true;
                    lutCh = keyCh;
                    lutAtt = keyAtt;
                    lutPh = keyPh;
                }
                if (e.channel != keyCh || e.att_code != keyAtt || e.phase_code != keyPh) {
                    continue;
                }
                lutFreq.push_back(static_cast<double>(e.freq_hz) / 1e9);
                lutMag.push_back(e.mag_db);
                lutPhaseErr.push_back(e.phase_error_deg);
            }
        }
    }
    {
        std::vector<afar::report::InverseLutEntry> rows;
        std::string diag;
        if (afar::report::readInverseLut(s.inverseLutPath(), rows, diag)) {
            inverseTotal = static_cast<qint64>(rows.size());
            int checked = 0;
            int flatish = 0;
            for (const auto& e : rows) {
                if (!e.valid) {
                    continue;
                }
                ++checked;
                // Плоский SIM: измеренные atten/phase около нуля при нулевых целях.
                if (std::fabs(e.measured_atten_db) < 0.05
                    && std::fabs(e.phase_residual_deg) < 0.05) {
                    ++flatish;
                }
                if (checked >= 32) {
                    break;
                }
            }
            inverseFlat = checked > 0 && flatish * 2 >= checked;
        }
    }

    const qint64 completedStates = m_orch->store().isOpen()
        ? static_cast<qint64>(m_orch->store().completedCount())
        : -1;
    const QString runId = QString::fromStdString(s.runId());

    m_artifactPreviewSent = true;
    emit seriesArtifactsPreview(runId, completedStates,
                                QString::fromStdString(s.directLutPath().string()), direct.count,
                                directTotal, directFlat, direct.fragment,
                                QString::fromStdString(s.inverseLutPath().string()), inverse.count,
                                inverseTotal, inverseFlat, inverse.fragment,
                                QString::fromStdString(s.reportPath().string()), report.count,
                                report.fragment,
                                QString::fromStdString(s.manifestPath().string()), manifest.count,
                                manifest.fragment);
    if (lutFreq.size() >= 2) {
        emit directLutCurvePreview(lutFreq, lutMag, lutPhaseErr, lutCh, lutAtt, lutPh);
    }
}

void MeasureWorker::emitPaths()
{
    if (!m_orch || m_orch->series().root().empty()) {
        return;
    }
    const auto& s = m_orch->series();
    emit pathsChanged(QString::fromStdString(s.root().string()),
                      QString::fromStdString(s.runConfigPath().string()),
                      QString::fromStdString(s.attenuatorCodesPath().string()),
                      QString::fromStdString(s.rawS21Path().string()),
                      QString::fromStdString(s.directLutPath().string()),
                      QString::fromStdString(s.inverseLutPath().string()),
                      QString::fromStdString(s.reportPath().string()),
                      QString::fromStdString(s.manifestPath().string()),
                      QString::fromStdString(s.runEventsPath().string()));
}

void MeasureWorker::emitMatrix(int channel, int attCode)
{
    if (!m_orch || !m_orch->store().isOpen()) {
        return;
    }
    const auto& store = m_orch->store();
    const auto& phases = store.phaseCodes();
    QVector<int> statuses;
    QVector<int> attempts;
    QVector<int> overloads;
    statuses.reserve(64);
    attempts.reserve(64);
    overloads.reserve(64);

    int measuringPhase = -1;
    const auto st = m_orch->state();
    const bool running = st == afar::RunState::Running || st == afar::RunState::Pausing
        || st == afar::RunState::Stopping;
    if (running && m_orch->cursor() < m_orch->scanOrder().size()) {
        const auto& cur = m_orch->scanOrder().at(m_orch->cursor()).state;
        if (cur.channel == static_cast<std::uint8_t>(channel)
            && cur.att_code == static_cast<std::uint16_t>(attCode)) {
            measuringPhase = cur.phase_code;
        }
    }

    for (int phase = 0; phase < 64; ++phase) {
        bool known = false;
        for (auto p : phases) {
            if (static_cast<int>(p) == phase) {
                known = true;
                break;
            }
        }
        if (!known) {
            statuses.push_back(CellOutOfAxis);
            attempts.push_back(0);
            overloads.push_back(0);
            continue;
        }
        if (phase == measuringPhase) {
            statuses.push_back(CellMeasuring);
            attempts.push_back(0);
            overloads.push_back(0);
            continue;
        }
        afar::RawS21StateRecord rec;
        std::string diag;
        if (!store.readState(static_cast<std::uint8_t>(channel),
                             static_cast<std::uint16_t>(attCode),
                             static_cast<std::uint8_t>(phase), rec, diag)) {
            statuses.push_back(CellWaiting);
            attempts.push_back(0);
            overloads.push_back(0);
            continue;
        }
        attempts.push_back(rec.attempt);
        overloads.push_back(rec.overload ? 1 : 0);
        if (rec.completed && rec.overload) {
            statuses.push_back(CellError);
        } else if (rec.completed) {
            statuses.push_back(CellDone);
        } else if (rec.attempt > 0 || rec.overload) {
            statuses.push_back(CellRetry);
        } else {
            statuses.push_back(CellWaiting);
        }
    }
    emit matrixSnapshot(channel, attCode, measuringPhase, statuses, attempts, overloads);
}

void MeasureWorker::prepare(const QString& dataRoot,
                            const QString& runConfigPath,
                            const QString& attenuatorCsvPath,
                            bool forceSafeState,
                            const QString& vnaCalibrationId)
{
    m_timer->stop();
    resetEta();
    m_sweepThrottleArmed = false;
    m_artifactPreviewSent = false;
    if (forceSafeState) {
        m_dut.set_safe_state();
    }
    std::string diag;
    afar::RunConfig cfg;
    afar::AttenuatorCodes att;
    if (!afar::RunConfig::loadFromFile(runConfigPath.toStdString(), cfg, diag)) {
        emit prepareFinished(false, QString::fromStdString(diag));
        emit diagnostic(QString::fromStdString(diag));
        emitState();
        return;
    }
    if (!afar::AttenuatorCodes::loadFromFile(attenuatorCsvPath.toStdString(), att, diag)) {
        emit prepareFinished(false, QString::fromStdString(diag));
        emit diagnostic(QString::fromStdString(diag));
        emitState();
        return;
    }

    // Новый оркестратор: повторный prepare из Idle после Complete/Error невозможен.
    m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    m_orch->setConfig(cfg, att);
    m_orch->setSleepEnabled(false);

    const bool ok = m_orch->prepare(dataRoot.toStdString(), runConfigPath.toStdString(),
                                    attenuatorCsvPath.toStdString(), diag,
                                    vnaCalibrationId.toStdString());
    emitConnection();
    emitState();
    if (ok) {
        emitPaths();
        emitProgress();
        emit etaChanged(QStringLiteral("ETA: —"));
        if (!m_orch->scanOrder().channels().empty()) {
            m_matrixChannel = m_orch->scanOrder().channels().front();
        }
        if (!m_orch->scanOrder().enabledAttCodes().empty()) {
            m_matrixAtt = m_orch->scanOrder().enabledAttCodes().front();
        }
        emitMatrix(m_matrixChannel, m_matrixAtt);
        QVector<int> channels;
        QVector<int> atts;
        for (auto c : m_orch->scanOrder().channels()) {
            channels.push_back(static_cast<int>(c));
        }
        for (auto a : m_orch->scanOrder().enabledAttCodes()) {
            atts.push_back(static_cast<int>(a));
        }
        emit axesChanged(channels, atts);
        emit diagnostic(QString());
    } else {
        emit diagnostic(QString::fromStdString(diag));
    }
    emitPendingScpiErrors();
    emit prepareFinished(ok, QString::fromStdString(diag));
}

void MeasureWorker::prepareRecovery(const QString& seriesDir)
{
    m_timer->stop();
    resetEta();
    m_sweepThrottleArmed = false;
    m_artifactPreviewSent = false;
    m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    m_orch->setSleepEnabled(false);
    std::string diag;
    const bool ok = m_orch->prepareRecovery(seriesDir.toStdString(), diag);
    emitConnection();
    emitState();
    if (ok) {
        emitPaths();
        emitProgress();
        emit etaChanged(QStringLiteral("ETA: —"));
        if (!m_orch->scanOrder().channels().empty()) {
            m_matrixChannel = m_orch->scanOrder().channels().front();
        }
        if (!m_orch->scanOrder().enabledAttCodes().empty()) {
            m_matrixAtt = m_orch->scanOrder().enabledAttCodes().front();
        }
        emitMatrix(m_matrixChannel, m_matrixAtt);
        QVector<int> channels;
        QVector<int> atts;
        for (auto c : m_orch->scanOrder().channels()) {
            channels.push_back(static_cast<int>(c));
        }
        for (auto a : m_orch->scanOrder().enabledAttCodes()) {
            atts.push_back(static_cast<int>(a));
        }
        emit axesChanged(channels, atts);
        emit diagnostic(QString());
    } else {
        emit diagnostic(QString::fromStdString(diag));
    }
    emitPendingScpiErrors();
    emit prepareFinished(ok, QString::fromStdString(diag));
}

void MeasureWorker::start()
{
    if (!m_orch) {
        return;
    }
    std::string diag;
    if (!m_orch->start(diag)) {
        emit diagnostic(QString::fromStdString(diag.empty() ? m_orch->lastError() : diag));
        emitState();
        return;
    }
    resetEta();
    emit diagnostic(QString());
    emitState();
    m_timer->start();
}

void MeasureWorker::pause()
{
    if (!m_orch) {
        return;
    }
    (void)m_orch->pause();
    emitState();
}

void MeasureWorker::resume()
{
    if (!m_orch) {
        return;
    }
    std::string diag;
    if (!m_orch->resume(diag)) {
        emit diagnostic(QString::fromStdString(diag));
    }
    resetEta();
    emitState();
    if (m_orch->state() == afar::RunState::Running) {
        m_timer->start();
    }
}

void MeasureWorker::stop()
{
    if (!m_orch) {
        return;
    }
    (void)m_orch->stop();
    resetEta();
    emitState();
}

void MeasureWorker::requestMatrixSnapshot(int channel, int attCode)
{
    m_matrixChannel = channel;
    m_matrixAtt = attCode;
    emitMatrix(channel, attCode);
}

void MeasureWorker::requestRemeasure(int channel, int attCode, const QVector<int>& phases)
{
    if (!m_orch) {
        emit diagnostic(QStringLiteral("Повтор: оркестратор не готов"));
        return;
    }
    std::vector<std::uint8_t> phaseList;
    phaseList.reserve(static_cast<std::size_t>(phases.size()));
    for (int p : phases) {
        if (p < 0 || p > 255) {
            continue;
        }
        phaseList.push_back(static_cast<std::uint8_t>(p));
    }
    std::string diag;
    if (!m_orch->requestRemeasure(static_cast<std::uint8_t>(channel),
                                  static_cast<std::uint16_t>(attCode), phaseList, diag)) {
        emit diagnostic(QString::fromStdString(
            diag.empty() ? std::string("Повтор не выполнен") : diag));
        emitState();
        return;
    }
    emit diagnostic(QString());
    m_matrixChannel = channel;
    m_matrixAtt = attCode;
    emitProgress();
    emitMatrix(channel, attCode);
    emitState();
}

void MeasureWorker::requestCellPreview(int channel, int attCode, int phase)
{
    // UI-03: UTC последнего STATE_OK из run-events.jsonl (не из слота store).
    QString slotUtc;
    if (m_orch && !m_orch->series().root().empty()) {
        std::vector<afar::RunEvent> events;
        std::string loadDiag;
        if (afar::RunEventLog::load(m_orch->series().runEventsPath(), events, loadDiag)) {
            for (const auto& ev : events) {
                if (ev.event_code != "STATE_OK") {
                    continue;
                }
                if (!ev.channel || !ev.att_code || !ev.phase_code) {
                    continue;
                }
                if (*ev.channel == channel && *ev.att_code == attCode
                    && *ev.phase_code == phase) {
                    slotUtc = QString::fromStdString(ev.timestamp_utc);
                }
            }
        }
    }
    emit cellSlotRecordedUtc(channel, attCode, phase, slotUtc);

    if (!m_orch || !m_orch->store().isOpen()) {
        emit diagnostic(QStringLiteral("Нет открытой серии — сначала мастер запуска"));
        return;
    }
    afar::RawS21StateRecord rec;
    std::string diag;
    if (!m_orch->store().readState(static_cast<std::uint8_t>(channel),
                                   static_cast<std::uint16_t>(attCode),
                                   static_cast<std::uint8_t>(phase), rec, diag)) {
        emit diagnostic(QString::fromStdString(
            diag.empty() ? std::string("Слот ещё не записан") : diag));
        return;
    }
    const auto& axis = m_orch->store().frequencyHz();
    const auto unwrap = afar::cal::unwrap_phase_deg(rec.s21);
    QVector<double> freqGhz;
    QVector<double> magDb;
    QVector<double> phaseDeg;
    const int n = static_cast<int>(rec.s21.size());
    freqGhz.reserve(n);
    magDb.reserve(n);
    phaseDeg.reserve(n);
    for (int i = 0; i < n; ++i) {
        double fGhz = 0.0;
        if (i < static_cast<int>(axis.size())) {
            fGhz = static_cast<double>(axis[static_cast<std::size_t>(i)]) / 1e9;
        }
        freqGhz.push_back(fGhz);
        magDb.push_back(afar::cal::magnitude_db(rec.s21[static_cast<std::size_t>(i)]));
        phaseDeg.push_back(i < static_cast<int>(unwrap.size())
                               ? unwrap[static_cast<std::size_t>(i)]
                               : 0.0);
    }
    emit cellSweepPreview(freqGhz, magDb, phaseDeg);
}

void MeasureWorker::shutdown()
{
    m_timer->stop();
    if (m_orch && (m_orch->state() == afar::RunState::Running
                   || m_orch->state() == afar::RunState::Paused
                   || m_orch->state() == afar::RunState::Pausing)) {
        (void)m_orch->stop();
        while (m_orch->stepOnce()) {
        }
    }
    emit finishedClean();
}

void MeasureWorker::onTick()
{
    if (!m_orch) {
        m_timer->stop();
        return;
    }
    // Несколько шагов за тик, но с отдачей управления GUI через queued timer.
    constexpr int kBurst = 4;
    bool progressed = false;
    for (int i = 0; i < kBurst; ++i) {
        if (!m_orch->stepOnce()) {
            break;
        }
        progressed = true;
    }
    emitConnection();
    emitState();
    emitProgress();
    maybeEmitSweepPreview();
    emitMatrix(m_matrixChannel, m_matrixAtt);
    emitPendingScpiErrors();

    const auto st = m_orch->state();
    if (st == afar::RunState::Complete) {
        emitArtifactPreviews();
    }
    if (st == afar::RunState::Paused || st == afar::RunState::Complete
        || st == afar::RunState::Aborted || st == afar::RunState::Error
        || st == afar::RunState::Ready || !progressed) {
        if (st != afar::RunState::Running && st != afar::RunState::Pausing
            && st != afar::RunState::Stopping && st != afar::RunState::Finalizing) {
            m_timer->stop();
        }
    }
}
