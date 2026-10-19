#include "StressTestPanel.h"

#include "PsSession.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

namespace ps = afar::ps;

namespace {

enum Column { ColCommand, ColSent, ColOk, ColRefused, ColTimeout, ColNotSent, ColAvg, ColMax, ColProblem, ColCount };

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString requestLine(const std::string& type, int count, const nlohmann::json& payload)
{
    return QString::fromStdString(
               ps::encodeLine(ps::makeCommand(type, static_cast<std::uint8_t>(count), payload)))
        .trimmed();
}

}  // namespace

StressTestPanel::StressTestPanel(PsSession* session, const QVector<Command>& commands, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    for (const Command& c : commands) {
        Stats s;
        s.cmd = c;
        m_stats.push_back(s);
    }

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(ui::kInnerGap);
    auto* controls = new QHBoxLayout;
    controls->setSpacing(ui::kInnerGap);
    m_count = new QSpinBox(this);
    m_count->setObjectName(QStringLiteral("stressCount"));
    m_count->setRange(1, 1'000'000);
    m_count->setValue(1000);
    m_count->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_duration = new QSpinBox(this);
    m_duration->setObjectName(QStringLiteral("stressDuration"));
    m_duration->setRange(1, 1'000'000);
    m_duration->setValue(10);
    m_duration->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_unit = new QComboBox(this);
    m_unit->setObjectName(QStringLiteral("stressUnit"));
    m_unit->addItem(QStringLiteral("мс"), 1);
    m_unit->addItem(QStringLiteral("секунд"), 1000);
    m_unit->addItem(QStringLiteral("минут"), 60000);
    m_unit->setCurrentIndex(1);
    m_start = new QPushButton(QStringLiteral("Запустить"), this);
    m_start->setObjectName(QStringLiteral("stressStart"));
    m_stop = new QPushButton(QStringLiteral("Остановить"), this);
    m_stop->setObjectName(QStringLiteral("stressStop"));
    m_stop->setEnabled(false);
    auto* reset = new QPushButton(QStringLiteral("Сбросить счётчики"), this);
    ui::setButtonRole(m_start, ui::ButtonRole::Primary);
    ui::setButtonRole(m_stop, ui::ButtonRole::Danger);
    ui::setButtonRole(reset, ui::ButtonRole::Secondary);
    controls->addWidget(new QLabel(QStringLiteral("Отправить команд"), this));
    controls->addWidget(m_count);
    controls->addWidget(new QLabel(QStringLiteral("за"), this));
    controls->addWidget(m_duration);
    controls->addWidget(m_unit);
    controls->addStretch(1);
    controls->addWidget(m_start);
    controls->addWidget(m_stop);
    controls->addWidget(reset);
    root->addLayout(controls);

    root->addWidget(ui::hintLabel(QStringLiteral("Выбранное число команд равномерно распределяется на заданное "
                                                 "время и идёт по кругу по отмеченным строкам, не дожидаясь "
                                                 "ответов. Сервисные команды PS выполняет только в сервисном "
                                                 "режиме, иначе — отказ. Клик по строке покажет последний "
                                                 "проблемный обмен этой команды."),
                                  this));

    m_warning = new QLabel(this);
    m_warning->setObjectName(QStringLiteral("stressWarning"));
    m_warning->setProperty("role", QStringLiteral("status"));
    m_warning->setWordWrap(true);
    ui::setTone(m_warning, ui::Tone::Warn);
    m_warning->setVisible(false);
    root->addWidget(m_warning);

    m_summary = new QLabel(QStringLiteral("Стресс-тест не запущен."), this);
    m_summary->setObjectName(QStringLiteral("stressSummary"));
    m_summary->setProperty("role", QStringLiteral("status"));
    m_summary->setWordWrap(true);
    root->addWidget(m_summary);

    m_table = new QTableWidget(static_cast<int>(m_stats.size()), ColCount, this);
    m_table->setObjectName(QStringLiteral("stressTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("Команда"), QStringLiteral("Отпр."),
                                        QStringLiteral("OK"), QStringLiteral("Отказ PS"),
                                        QStringLiteral("Нет ответа"), QStringLiteral("Не ушло"),
                                        QStringLiteral("Ср. мс"), QStringLiteral("Макс. мс"),
                                        QStringLiteral("Последняя проблема")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setMinimumHeight(180);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColProblem, QHeaderView::Stretch);
    for (int r = 0; r < m_stats.size(); ++r) {
        for (int c = 0; c < ColCount; ++c) {
            m_table->setItem(r, c, new QTableWidgetItem);
        }
        QTableWidgetItem* name = m_table->item(r, ColCommand);
        const Stats& s = m_stats.at(r);
        name->setText(s.cmd.payload.empty() ? QString::fromStdString(s.cmd.msgType)
                                            : QStringLiteral("%1 %2").arg(QString::fromStdString(s.cmd.msgType),
                                                                          QString::fromStdString(s.cmd.payload.dump())));
        name->setFlags(name->flags() | Qt::ItemIsUserCheckable);
        name->setCheckState(s.cmd.setter ? Qt::Unchecked : Qt::Checked);
        m_stats[r].enabled = !s.cmd.setter;
    }
    root->addWidget(m_table, 1);

    m_sendTimer = new QTimer(this);
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(250);

    PsLink* link = m_session->link();
    connect(m_start, &QPushButton::clicked, this, &StressTestPanel::start);
    connect(m_stop, &QPushButton::clicked, this, [this] { stop(QStringLiteral("остановлено оператором")); });
    connect(reset, &QPushButton::clicked, this, &StressTestPanel::resetCounters);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        if (item->column() == ColCommand && item->row() < m_stats.size()) {
            m_stats[item->row()].enabled = item->checkState() == Qt::Checked;
            updateWarning();
        }
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row < 0 || row >= m_stats.size()) {
            return;
        }
        const Stats& s = m_stats.at(row);
        const QString type = QString::fromStdString(s.cmd.msgType);
        if (!s.problemRequest.isEmpty()) {
            emit exchangeChosen(QStringLiteral("%1 — последняя проблема: %2").arg(type, s.lastProblem),
                                s.problemRequest, s.problemResponse);
        } else if (!s.lastRequest.isEmpty()) {
            emit exchangeChosen(QStringLiteral("%1 — последний обмен, проблем не было").arg(type),
                                s.lastRequest, s.lastResponse);
        }
    });
    connect(m_sendTimer, &QTimer::timeout, this, &StressTestPanel::sendNext);
    connect(m_refreshTimer, &QTimer::timeout, this, &StressTestPanel::refresh);
    connect(link, &PsLink::responseReceived, this, &StressTestPanel::onResponse);
    connect(link, &PsLink::commandTimedOut, this, &StressTestPanel::onTimeout);
    connect(link, &PsLink::stateChanged, this, &StressTestPanel::onLinkState);
    updateWarning();
    refresh();
}

QString StressTestPanel::key(const QString& msgType, int msgCount)
{
    return msgType + QLatin1Char('#') + QString::number(msgCount);
}

int StressTestPanel::totalSent() const
{
    int n = 0;
    for (const Stats& s : m_stats) {
        n += s.sent + s.notSent;
    }
    return n;
}

bool StressTestPanel::rowInFlight(int row) const
{
    for (const InFlight& f : m_inFlight) {
        if (f.row == row) {
            return true;
        }
    }
    return false;
}

void StressTestPanel::start()
{
    bool any = false;
    for (const Stats& s : m_stats) {
        any = any || s.enabled;
    }
    if (!any) {
        m_summary->setText(QStringLiteral("Не выбрано ни одной команды — отметьте галочки в таблице."));
        return;
    }
    if (!m_session->manualAllowed()) {
        m_summary->setText(QStringLiteral("Нет связи с PS — сначала подключитесь на вкладке «Настройки»."));
        return;
    }
    resetCounters();
    m_running = true;
    m_finishing = false;
    m_stopReason.clear();
    m_runClock.start();
    m_start->setEnabled(false);
    m_stop->setEnabled(true);
    m_plannedCount = m_count->value();
    m_plannedMs = durationMs();
    m_sendTimer->setInterval(5);  // команды набегают по часам, а не по одной за такт: пока 1000 за секунду
    m_sendTimer->start();
    m_refreshTimer->start();
    emit runningChanged(true);
    refresh();
}

void StressTestPanel::stop(const QString& why)
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_finishing = false;
    m_stopReason = why;
    m_sendTimer->stop();
    m_refreshTimer->stop();
    m_start->setEnabled(true);
    m_stop->setEnabled(false);
    refresh();
    emit runningChanged(false);
}

void StressTestPanel::resetCounters()
{
    for (Stats& s : m_stats) {
        const Command cmd = s.cmd;
        const bool enabled = s.enabled;
        s = Stats{};
        s.cmd = cmd;
        s.enabled = enabled;
    }
    m_inFlight.clear();
    m_cursor = 0;
    if (!m_running) {
        m_stopReason.clear();
    }
    refresh();
}

qint64 StressTestPanel::durationMs() const
{
    return static_cast<qint64>(m_duration->value()) * m_unit->currentData().toInt();
}

void StressTestPanel::updateWarning()
{
    QStringList names;
    for (const Stats& s : m_stats) {
        if (s.enabled && s.cmd.setter) {
            names << QString::fromStdString(s.cmd.msgType);
        }
    }
    names.removeDuplicates();
    m_warning->setVisible(!names.isEmpty());
    if (!names.isEmpty()) {
        m_warning->setText(QStringLiteral("Внимание: отмечены команды, которые меняют настройки платы (%1). "
                                          "Они уходят с пустой нагрузкой, поэтому PS, как правило, ответит "
                                          "отказом; на реальной плате включайте их осознанно.")
                               .arg(names.join(QStringLiteral(", "))));
    }
}

bool StressTestPanel::sendOne()
{
    // Следующая включённая команда по кругу; ответов не ждём.
    for (int step = 0; step < m_stats.size(); ++step) {
        const int i = (m_cursor + step) % static_cast<int>(m_stats.size());
        Stats& s = m_stats[i];
        if (!s.enabled) {
            continue;
        }
        m_cursor = (i + 1) % static_cast<int>(m_stats.size());
        // msg_count — один байт: больше ~150 ожидающих ответа нельзя, иначе ключи совпадут.
        if (m_inFlight.size() >= 150) {
            ++s.notSent;
            markProblem(s, QStringLiteral("не ушло: PS не успевает отвечать (150 команд без ответа)"),
                        requestLine(s.cmd.msgType, 0, s.cmd.payload), {});
            return true;
        }
        const int count = m_session->send(s.cmd.msgType, s.cmd.payload);
        if (count >= 0) {
            InFlight f;
            f.row = i;
            f.clock.start();
            f.request = requestLine(s.cmd.msgType, count, s.cmd.payload);
            ++s.sent;
            s.lastRequest = f.request;
            s.lastResponse.clear();
            m_inFlight.insert(key(QString::fromStdString(s.cmd.msgType), count), f);
        } else {
            ++s.notSent;
            markProblem(s, QStringLiteral("не ушло: %1").arg(m_session->lastError()),
                        requestLine(s.cmd.msgType, 0, s.cmd.payload), {});
        }
        return true;
    }
    return false;
}

void StressTestPanel::sendNext()
{
    if (!m_running || m_finishing) {
        return;
    }
    if (!m_session->link()->isReady()) {
        stop(QStringLiteral("связь с PS потеряна"));
        return;
    }
    // Сколько команд должно было уйти к этому моменту; за один такт — не больше 200, чтобы не вешать окно.
    const qint64 elapsed = m_runClock.elapsed();
    const qint64 due = std::min<qint64>(
        m_plannedCount, 1 + elapsed * m_plannedCount / std::max<qint64>(1, m_plannedMs));
    int budget = 200;
    while (totalSent() < due && budget-- > 0) {
        if (!sendOne()) {
            break;
        }
    }
    if (totalSent() >= m_plannedCount) {
        m_finishing = true;
        m_sendTimer->stop();
        finishIfDone();
    }
}

void StressTestPanel::onResponse(const ps::Envelope& r, ps::Status status)
{
    const auto it = m_inFlight.find(key(qs(r.msg_type), r.msg_count));
    if (it == m_inFlight.end()) {
        return;  // не наш обмен: кнопки вкладок, keep_alive, одиночные команды
    }
    const InFlight f = it.value();
    m_inFlight.erase(it);
    Stats& s = m_stats[f.row];
    const qint64 ms = f.clock.elapsed();
    s.totalMs += ms;
    s.maxMs = std::max(s.maxMs, ms);
    s.lastResponse = QString::fromStdString(ps::encodeLine(r)).trimmed();
    if (status == ps::Status::Ok) {
        ++s.ok;
    } else {
        ++s.refused;
        markProblem(s, QStringLiteral("PS отказал: %1").arg(qs(ps::toString(status))), f.request, s.lastResponse);
    }
    finishIfDone();
}

void StressTestPanel::onTimeout(const QString& msgType, int msgCount)
{
    const auto it = m_inFlight.find(key(msgType, msgCount));
    if (it == m_inFlight.end()) {
        return;
    }
    const InFlight f = it.value();
    m_inFlight.erase(it);
    Stats& s = m_stats[f.row];
    ++s.timeout;
    markProblem(s, QStringLiteral("нет ответа за %1 мс").arg(m_session->link()->timeouts().commandMs),
                f.request, {});
    finishIfDone();
}

void StressTestPanel::onLinkState()
{
    if (m_session->link()->isReady() || !m_running) {
        return;
    }
    for (const InFlight& f : m_inFlight) {
        Stats& s = m_stats[f.row];
        ++s.timeout;
        markProblem(s, QStringLiteral("связь с PS прервалась до ответа"), f.request, {});
    }
    m_inFlight.clear();
    stop(QStringLiteral("связь с PS прервалась"));
}

void StressTestPanel::finishIfDone()
{
    if (m_finishing && m_inFlight.isEmpty()) {
        stop(QStringLiteral("отправлено %1 команд").arg(m_plannedCount));
    }
}

void StressTestPanel::markProblem(Stats& s, const QString& text, const QString& request, const QString& response)
{
    s.lastProblem = QStringLiteral("%1 · %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), text);
    s.problemRequest = request;
    s.problemResponse = response;
}

QString StressTestPanel::summary() const
{
    return m_summary->text();
}

void StressTestPanel::refresh()
{
    int sent = 0;
    int ok = 0;
    int refused = 0;
    int timeout = 0;
    int notSent = 0;
    qint64 totalMs = 0;
    qint64 maxMs = 0;
    const auto num = [](int v) { return v ? QString::number(v) : QStringLiteral("·"); };
    for (int r = 0; r < m_stats.size(); ++r) {
        const Stats& s = m_stats.at(r);
        sent += s.sent;
        ok += s.ok;
        refused += s.refused;
        timeout += s.timeout;
        notSent += s.notSent;
        totalMs += s.totalMs;
        maxMs = std::max(maxMs, s.maxMs);
        const int answered = s.ok + s.refused;
        m_table->item(r, ColSent)->setText(num(s.sent));
        m_table->item(r, ColOk)->setText(num(s.ok));
        m_table->item(r, ColRefused)->setText(num(s.refused));
        m_table->item(r, ColTimeout)->setText(num(s.timeout));
        m_table->item(r, ColNotSent)->setText(num(s.notSent));
        m_table->item(r, ColAvg)->setText(answered ? QString::number(s.totalMs / answered) : QStringLiteral("·"));
        m_table->item(r, ColMax)->setText(answered ? QString::number(s.maxMs) : QStringLiteral("·"));
        m_table->item(r, ColProblem)->setText(s.lastProblem);
        m_table->item(r, ColProblem)->setForeground(s.timeout || s.notSent ? QBrush(ui::errorColor()) : QBrush(ui::warnColor()));
    }
    const int answered = ok + refused;
    const int limit = m_running ? m_plannedCount : m_count->value();
    QString counts = QStringLiteral("отправлено %1%2 · OK %3 · отказ PS %4 · нет ответа %5 · не ушло %6")
                         .arg(sent)
                         .arg(limit > 0 ? QStringLiteral(" из %1").arg(limit) : QString())
                         .arg(ok)
                         .arg(refused)
                         .arg(timeout)
                         .arg(notSent);
    if (answered) {
        counts += QStringLiteral(" · ответ в среднем %1 мс, максимум %2 мс").arg(totalMs / answered).arg(maxMs);
    }
    const double secs = m_runClock.isValid() ? double(m_runClock.elapsed()) / 1000.0 : 0.0;
    if (m_running) {
        m_summary->setText(QStringLiteral("Идёт%1: %2 · ждём ответа %3 · фактически %4 команд/с")
                               .arg(m_finishing ? QStringLiteral(" (дожидаемся последних ответов)") : QString(), counts)
                               .arg(m_inFlight.size())
                               .arg(secs > 0 ? QString::number(sent / secs, 'f', 1) : QStringLiteral("0")));
    } else if (!m_stopReason.isEmpty()) {
        m_summary->setText(QStringLiteral("Остановлено (%1). Итог: %2").arg(m_stopReason, counts));
    } else {
        m_summary->setText(QStringLiteral("Стресс-тест не запущен."));
    }
    ui::setTone(m_summary, timeout || notSent ? ui::Tone::Error
                           : refused         ? ui::Tone::Warn
                           : m_running       ? ui::Tone::Info
                           : ok > 0          ? ui::Tone::Ok
                                             : ui::Tone::Neutral);
}
