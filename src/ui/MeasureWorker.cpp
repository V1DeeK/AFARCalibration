#include "MeasureWorker.h"

#include "AttenuatorCodes.h"
#include "C2220Vna.h"
#include "PhaseMath.h"
#include "RawS21Store.h"
#include "RunConfig.h"
#include "ScpiComTransport.h"
#include "ScpiSocketTransport.h"

#include <QTimer>

#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

    if (m_backend == BackendSocket) {
        m_socket = std::make_unique<ScpiSocketTransport>(m_host.toStdString(),
                                                          static_cast<std::uint16_t>(m_port));
        C2220Vna::Profile profile;
        profile.allow_direct_access = m_allowDirect;
        m_c2220 = std::make_unique<C2220Vna>(*m_socket, profile);
        m_vna = m_c2220.get();
    } else if (m_backend == BackendCom) {
        m_com = std::make_unique<ScpiComTransport>(m_comPort.toStdString());
        C2220Vna::Profile profile;
        profile.allow_direct_access = m_allowDirect;
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

void MeasureWorker::configureVna(int backend,
                                 const QString& host,
                                 int port,
                                 const QString& comPort,
                                 bool allowDirectAccess)
{
    m_timer->stop();
    m_orch.reset();
    m_backend = backend;
    m_host = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    m_port = (port > 0 && port < 65536) ? port : 5025;
    m_comPort = comPort.trimmed().isEmpty() ? QStringLiteral("COM3") : comPort.trimmed();
    m_allowDirect = allowDirectAccess;
    rebuildVna();
    m_orch = std::make_unique<afar::MeasurementOrchestrator>(activeVna(), &m_dut);
    emitConnection();
    emit diagnostic(QString());
}

void MeasureWorker::probeVna()
{
    if (!m_vna) {
        emit probeFinished(false, QStringLiteral("VNA не сконфигурирован"));
        return;
    }
    try {
        m_vna->connect();
        const QString idn = QString::fromStdString(m_vna->identify());
        emitConnection();
        emit probeFinished(true, idn);
        emit diagnostic(QStringLiteral("IDN: %1").arg(idn));
    } catch (const std::exception& ex) {
        emitConnection();
        emit probeFinished(false, QString::fromUtf8(ex.what()));
        emit diagnostic(QString::fromUtf8(ex.what()));
    }
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
    QString address = QStringLiteral("не задан");
    QString iface = QStringLiteral("имитатор");
    double temp = 0.0;
    bool tempOk = false;
    bool vnaOk = false;
    if (m_dut.connected()) {
        temp = m_dut.temperature_c();
        tempOk = true;
        iface = QStringLiteral("DutSimulator");
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
    emit connectionChanged(model, address, vnaOk, iface, m_dut.connected(), temp, tempOk);
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

    const auto& sweep = m_orch->lastMeasuredSweep();
    const auto unwrap = afar::cal::unwrap_phase_deg(sweep.s21);
    QVector<double> freqGhz;
    QVector<double> magDb;
    QVector<double> phaseDeg;
    const int n = static_cast<int>(sweep.s21.size());
    freqGhz.reserve(n);
    magDb.reserve(n);
    phaseDeg.reserve(n);
    for (int i = 0; i < n; ++i) {
        double fGhz = 0.0;
        if (i < static_cast<int>(sweep.frequency_hz.size())) {
            fGhz = static_cast<double>(sweep.frequency_hz[static_cast<std::size_t>(i)]) / 1e9;
        }
        freqGhz.push_back(fGhz);
        magDb.push_back(afar::cal::magnitude_db(sweep.s21[static_cast<std::size_t>(i)]));
        phaseDeg.push_back(i < static_cast<int>(unwrap.size())
                               ? unwrap[static_cast<std::size_t>(i)]
                               : 0.0);
    }
    emit sweepPreview(freqGhz, magDb, phaseDeg);
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
                            bool forceSafeState)
{
    m_timer->stop();
    resetEta();
    m_sweepThrottleArmed = false;
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
                                    attenuatorCsvPath.toStdString(), diag);
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
    emit prepareFinished(ok, QString::fromStdString(diag));
}

void MeasureWorker::prepareRecovery(const QString& seriesDir)
{
    m_timer->stop();
    resetEta();
    m_sweepThrottleArmed = false;
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

    const auto st = m_orch->state();
    if (st == afar::RunState::Paused || st == afar::RunState::Complete
        || st == afar::RunState::Aborted || st == afar::RunState::Error
        || st == afar::RunState::Ready || !progressed) {
        if (st != afar::RunState::Running && st != afar::RunState::Pausing
            && st != afar::RunState::Stopping && st != afar::RunState::Finalizing) {
            m_timer->stop();
        }
    }
}
