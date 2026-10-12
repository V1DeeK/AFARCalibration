#include "PsServiceCheckRunner.h"

namespace ps = afar::ps;
using nlohmann::json;

namespace {

/// Период телеметрии 1 с (§5) плюс запас на внеочередную датаграмму.
constexpr int kUdpWaitMs = 3500;

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QString jsonText(const json& j)
{
    return QString::fromStdString(j.dump(-1, ' ', false, json::error_handler_t::replace));
}

QString cell(QString s)
{
    return s.replace(QLatin1Char('|'), QStringLiteral("\\|")).replace(QLatin1Char('\n'), QLatin1Char(' '));
}

}  // namespace

PsServiceCheckRunner::PsServiceCheckRunner(PsLink* link, QObject* parent)
    : QObject(parent)
    , m_link(link)
    , m_steps(ps::serviceCheckSteps())
    , m_outcomes(m_steps.size())
{
    m_guard.setSingleShot(true);
    connect(&m_guard, &QTimer::timeout, this, [this] {
        if (!isRunning()) {
            return;
        }
        if (m_waitUdp) {
            m_waitUdp = false;
            if (!m_lastUdpText.isEmpty()) {
                finishStep(m_lastUdp, m_lastUdpText, QString());
            } else {
                finishStep({ps::CheckVerdict::NoAnswer,
                            "за " + std::to_string(kUdpWaitMs) + " мс не пришло ни одной датаграммы на UDP " +
                                std::to_string(m_link->telemetryPort())},
                           QStringLiteral("нет датаграмм"), QString());
            }
            return;
        }
        finishStep({ps::CheckVerdict::NoAnswer, "ответ не пришёл или не разобран"},
                   QStringLiteral("нет ответа"), QString());
    });
    connect(m_link, &PsLink::responseReceived, this, &PsServiceCheckRunner::onResponse);
    connect(m_link, &PsLink::commandTimedOut, this, &PsServiceCheckRunner::onTimedOut);
    connect(m_link, &PsLink::stateChanged, this, &PsServiceCheckRunner::onLinkState);
    connect(m_link, &PsLink::frameLogged, this, &PsServiceCheckRunner::onFrame);
}

bool PsServiceCheckRunner::start()
{
    if (isRunning() || !m_link->isReady()) {
        return false;
    }
    m_outcomes.assign(m_steps.size(), Outcome{});
    m_startedAt = QDateTime::currentDateTime();
    m_finishedAt = {};
    m_finishReason.clear();
    m_tlmReceived0 = m_link->telemetryPackets();
    m_tlmLost0 = m_link->telemetryLost();
    m_tlmRejected0 = m_link->telemetryRejected();
    m_index = 0;
    sendCurrent();
    return true;
}

void PsServiceCheckRunner::stop(const QString& reason)
{
    if (!isRunning()) {
        return;
    }
    m_guard.stop();
    m_index = -1;
    m_msgCount = -1;
    m_waitUdp = false;
    m_finishedAt = QDateTime::currentDateTime();
    m_finishReason = reason;
    emit finished(reason);
}

void PsServiceCheckRunner::sendCurrent()
{
    const auto& step = m_steps[static_cast<std::size_t>(m_index)];
    auto& out = m_outcomes[static_cast<std::size_t>(m_index)];
    out.request = step.udp == ps::UdpCheck::None ? jsonText(step.payload) : QString();
    emit stepStarted(m_index);
    m_stepClock.start();
    if (step.udp == ps::UdpCheck::Counters) {
        checkCounters();
        return;
    }
    if (step.udp == ps::UdpCheck::Datagram) {
        m_waitUdp = true;
        m_lastUdp = {};
        m_lastUdpText.clear();
        m_guard.start(kUdpWaitMs);
        return;
    }
    m_msgCount = m_link->sendCommand(step.msgType, step.payload);
    if (m_msgCount < 0) {
        finishStep({ps::CheckVerdict::Fail, "команда не ушла: нет связи с PS"},
                   QStringLiteral("не отправлено"), QString());
        return;
    }
    // Страховка на случай ответа без корректного status: PsLink его не передаёт.
    m_guard.start(m_link->timeouts().commandMs + 1000);
}

void PsServiceCheckRunner::finishStep(ps::CheckResult result, const QString& got, const QString& response)
{
    m_guard.stop();
    const int index = m_index;
    auto& out = m_outcomes[static_cast<std::size_t>(index)];
    out.result = std::move(result);
    out.got = got;
    out.response = response;
    out.ms = m_stepClock.elapsed();
    m_msgCount = -1;
    emit stepFinished(index);
    if (m_index != index) {
        return;  // остановили из обработчика
    }
    if (index + 1 >= static_cast<int>(m_steps.size())) {
        stop(QStringLiteral("все шаги выполнены"));
        return;
    }
    // Следующий шаг — из цикла событий, а не из обработчика ответа.
    QTimer::singleShot(0, this, [this, index] {
        if (m_index == index) {
            m_index = index + 1;
            sendCurrent();
        }
    });
}

void PsServiceCheckRunner::onResponse(const ps::Envelope& response, ps::Status status)
{
    if (!isRunning() || m_msgCount < 0 || response.msg_count != m_msgCount) {
        return;
    }
    const auto& step = m_steps[static_cast<std::size_t>(m_index)];
    if (response.msg_type != step.msgType) {
        return;
    }
    finishStep(ps::evaluate(step, status, response.payload), qs(ps::responseText(status, response.payload)),
               jsonText(response.payload));
}

void PsServiceCheckRunner::onTimedOut(const QString& msgType, int msgCount)
{
    if (!isRunning() || msgCount != m_msgCount ||
        msgType != qs(m_steps[static_cast<std::size_t>(m_index)].msgType)) {
        return;
    }
    finishStep({ps::CheckVerdict::NoAnswer,
                "PS не ответил за " + std::to_string(m_link->timeouts().commandMs) + " мс"},
               QStringLiteral("нет ответа"), QString());
}

void PsServiceCheckRunner::onFrame(bool outgoing, const QString& channel, const QString& line)
{
    if (!isRunning() || !m_waitUdp || outgoing || channel != QLatin1String("UDP")) {
        return;
    }
    const std::string datagram = line.toStdString();
    const auto& step = m_steps[static_cast<std::size_t>(m_index)];
    bool waitMore = false;
    ps::CheckResult r = ps::evaluateTelemetry(step, datagram, &waitMore);
    const QString text = qs(ps::telemetryText(datagram));
    if (waitMore) {
        m_lastUdp = std::move(r);
        m_lastUdpText = text;
        return;
    }
    m_waitUdp = false;
    finishStep(std::move(r), text, line);
}

void PsServiceCheckRunner::checkCounters()
{
    const quint64 received = m_link->telemetryPackets() - m_tlmReceived0;
    const quint64 lost = m_link->telemetryLost() - m_tlmLost0;
    const quint64 rejected = m_link->telemetryRejected() - m_tlmRejected0;
    const QString got = QStringLiteral("принято %1, потеряно %2, отброшено %3").arg(received).arg(lost).arg(rejected);
    ps::CheckResult r{ps::CheckVerdict::Pass, {}};
    if (received == 0) {
        r = {ps::CheckVerdict::Fail, "за проверку не принято ни одной датаграммы"};
    } else if (lost > 0 || rejected > 0) {
        r = {ps::CheckVerdict::Fail, "потеряно " + std::to_string(lost) + ", отброшено " + std::to_string(rejected)};
    }
    // Итог — из цикла событий, как у остальных шагов.
    QTimer::singleShot(0, this, [this, r, got, index = m_index] {
        if (m_index == index) {
            finishStep(r, got, QString());
        }
    });
}

void PsServiceCheckRunner::onLinkState(PsLink::State state)
{
    if (!isRunning() || state == PsLink::State::Ready) {
        return;
    }
    auto& out = m_outcomes[static_cast<std::size_t>(m_index)];
    out.result = {ps::CheckVerdict::NoAnswer, "связь с PS прервалась до ответа"};
    out.got = QStringLiteral("связь потеряна");
    const int index = m_index;
    m_guard.stop();
    emit stepFinished(index);
    stop(QStringLiteral("связь с PS прервалась на шаге %1").arg(index + 1));
}

QString PsServiceCheckRunner::reportMarkdown() const
{
    int pass = 0;
    int fail = 0;
    int run = 0;
    for (const auto& o : m_outcomes) {
        if (o.result.verdict != ps::CheckVerdict::NotRun) {
            ++run;
        }
        if (o.result.verdict == ps::CheckVerdict::Pass) {
            ++pass;
        } else if (o.result.verdict != ps::CheckVerdict::NotRun) {
            ++fail;
        }
    }
    QString md = QStringLiteral("# Приёмочная проверка сервисного режима PS\n\n");
    md += QStringLiteral("| | |\n|---|---|\n");
    md += QStringLiteral("| PS | %1:%2 |\n").arg(m_link->host()).arg(m_link->port());
    md += QStringLiteral("| Версия протокола PS | %1 |\n")
              .arg(m_link->peerVersion().isEmpty() ? QStringLiteral("—") : m_link->peerVersion());
    md += QStringLiteral("| Начало | %1 |\n").arg(m_startedAt.toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")));
    md += QStringLiteral("| Окончание | %1 (%2) |\n")
              .arg(m_finishedAt.toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")), m_finishReason);
    md += QStringLiteral("| Итог | пройдено %1 из %2, не пройдено %3, выполнено %4 |\n\n")
              .arg(pass)
              .arg(m_steps.size())
              .arg(fail)
              .arg(run);
    md += QStringLiteral("Контракт: docs/contracts/ps-json-protocol.md. «предл.» — шаг проверяет "
                         "предложение контракта, ещё не согласованное с разработчиком PS.\n\n");
    md += QStringLiteral("| № | Этап | Проверка | Команда | Ожидали | Получили | Итог | мс |\n");
    md += QStringLiteral("|---|---|---|---|---|---|---|---|\n");
    for (std::size_t i = 0; i < m_steps.size(); ++i) {
        const auto& s = m_steps[i];
        const auto& o = m_outcomes[i];
        QString verdict = qs(ps::verdictRu(o.result.verdict));
        if (!o.result.detail.empty()) {
            verdict += QStringLiteral(": ") + qs(o.result.detail);
        }
        md += QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 | %7 | %8 |\n")
                  .arg(i + 1)
                  .arg(cell(qs(s.phase)),
                       cell(qs(s.title) + (s.proposal ? QStringLiteral(" (предл.)") : QString())),
                       (s.udp == ps::UdpCheck::None ? QStringLiteral("TCP ") : QStringLiteral("UDP ")) + qs(s.msgType),
                       cell(qs(ps::expectedText(s))), cell(o.got), cell(verdict),
                       o.result.verdict == ps::CheckVerdict::NotRun ? QStringLiteral("—")
                                                                    : QString::number(o.ms));
    }
    return md;
}
