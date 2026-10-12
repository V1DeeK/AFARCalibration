#include "PsTrafficView.h"

#include "PsLink.h"

#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace ps = afar::ps;

namespace {

constexpr std::size_t kMaxFrames = 20000;

enum Column { ColTime, ColChannel, ColDir, ColType, ColCount, ColClass, ColStatus, ColMs, ColBytes, ColCountAll };

QString pretty(const QString& line)
{
    const auto j = nlohmann::json::parse(line.toStdString(), nullptr, false);
    return j.is_discarded() ? line : QString::fromStdString(j.dump(2));
}

}  // namespace

PsTrafficView::PsTrafficView(PsLink* link, QWidget* parent)
    : QWidget(parent)
    , m_link(link)
{
    m_clock.start();
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);

    auto* bar = new QHBoxLayout;
    m_showTcp = new QCheckBox(QStringLiteral("TCP: команды и ответы"), this);
    m_showTcp->setChecked(true);
    m_showUdp = new QCheckBox(QStringLiteral("UDP: телеметрия"), this);
    m_showUdp->setChecked(true);
    m_showKeepAlive = new QCheckBox(QStringLiteral("keep_alive"), this);
    m_follow = new QCheckBox(QStringLiteral("Следить за новыми"), this);
    m_follow->setChecked(true);
    auto* save = new QPushButton(QStringLiteral("Сохранить обмен…"), this);
    auto* clearBtn = new QPushButton(QStringLiteral("Очистить"), this);
    bar->addWidget(m_showTcp);
    bar->addWidget(m_showUdp);
    bar->addWidget(m_showKeepAlive);
    bar->addWidget(m_follow);
    bar->addStretch(1);
    bar->addWidget(save);
    bar->addWidget(clearBtn);
    root->addLayout(bar);

    m_counters = new QLabel(this);
    m_counters->setObjectName(QStringLiteral("psTrafficCounters"));
    m_counters->setTextInteractionFlags(Qt::TextSelectableByMouse);
    root->addWidget(m_counters);

    auto* split = new QSplitter(Qt::Horizontal, this);
    m_table = new QTableWidget(0, ColCountAll, split);
    m_table->setObjectName(QStringLiteral("psTrafficTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("Время"), QStringLiteral("Канал"), QStringLiteral("Куда"),
                                        QStringLiteral("msg_type"), QStringLiteral("№"), QStringLiteral("Класс"),
                                        QStringLiteral("status"), QStringLiteral("мс"), QStringLiteral("байт")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColType, QHeaderView::Stretch);

    auto* detail = new QWidget(split);
    auto* dl = new QVBoxLayout(detail);
    dl->setContentsMargins(0, 0, 0, 0);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_sentTitle = new QLabel(QStringLiteral("Отправлено"), detail);
    m_sent = new QPlainTextEdit(detail);
    m_sent->setObjectName(QStringLiteral("psTrafficSent"));
    m_receivedTitle = new QLabel(QStringLiteral("Получено"), detail);
    m_received = new QPlainTextEdit(detail);
    m_received->setObjectName(QStringLiteral("psTrafficReceived"));
    for (QPlainTextEdit* e : {m_sent, m_received}) {
        e->setReadOnly(true);
        e->setFont(mono);
    }
    m_sent->setPlaceholderText(QStringLiteral("выберите строку обмена"));
    dl->addWidget(m_sentTitle);
    dl->addWidget(m_sent, 1);
    dl->addWidget(m_receivedTitle);
    dl->addWidget(m_received, 1);
    split->addWidget(m_table);
    split->addWidget(detail);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    root->addWidget(split, 1);

    m_countersTimer = new QTimer(this);
    m_countersTimer->setInterval(500);
    m_countersTimer->start();

    for (QCheckBox* box : {m_showTcp, m_showUdp, m_showKeepAlive}) {
        connect(box, &QCheckBox::toggled, this, &PsTrafficView::rebuildTable);
    }
    connect(save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Сохранить обмен с PS"),
            QStringLiteral("ps-exchange-%1.log").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))),
            QStringLiteral("Журнал (*.log *.txt)"));
        QString error;
        if (!path.isEmpty() && !saveLog(path, &error)) {
            m_counters->setText(QStringLiteral("Не сохранено: %1").arg(error));
        }
    });
    connect(clearBtn, &QPushButton::clicked, this, &PsTrafficView::clear);
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row >= 0 && m_table->item(row, ColTime)) {
            showFrame(m_table->item(row, ColTime)->data(Qt::UserRole).toInt());
        }
    });
    connect(m_link, &PsLink::frameLogged, this, &PsTrafficView::onFrame);
    connect(m_link, &PsLink::commandTimedOut, this, &PsTrafficView::onTimedOut);
    connect(m_link, &PsLink::stateChanged, this, [this](PsLink::State s) {
        if (s == PsLink::State::Disconnected) {
            // Ответы на эти команды уже не придут: PsLink их не дождётся.
            m_openCommands.clear();
        }
        refreshCounters();
    });
    connect(m_countersTimer, &QTimer::timeout, this, &PsTrafficView::refreshCounters);
    refreshCounters();
}

QString PsTrafficView::key(const QString& type, int count)
{
    return type + QLatin1Char('#') + QString::number(count);
}

void PsTrafficView::onFrame(bool outgoing, const QString& channel, const QString& line)
{
    if (m_frames.size() >= kMaxFrames) {
        clear();
    }
    Frame f;
    f.at = QDateTime::currentDateTime();
    f.clockMs = m_clock.elapsed();
    f.outgoing = outgoing;
    f.udp = channel == QLatin1String("UDP");
    f.line = line;
    ps::Envelope env;
    std::string error;
    if (ps::decodeLine(line.toStdString(), env, &error)) {
        f.msgType = QString::fromStdString(env.msg_type);
        f.msgClass = QString::fromUtf8(ps::toString(env.msg_class).data());
        f.msgCount = env.msg_count;
        if (const auto it = env.payload.find("status"); it != env.payload.end() && it->is_string()) {
            f.status = QString::fromStdString(it->get<std::string>());
        } else if (env.msg_class == ps::MsgClass::Response) {
            f.status = QStringLiteral("нет status");
        }
    } else {
        f.broken = true;
        f.status = QStringLiteral("битый пакет: %1").arg(QString::fromStdString(error));
    }

    const int index = static_cast<int>(m_frames.size());
    if (f.udp) {
        ++m_udpReceived;
    } else if (outgoing) {
        ++m_tcpSent;
        if (!f.broken) {
            m_openCommands.insert(key(f.msgType, f.msgCount), index);
        }
    } else {
        ++m_tcpReceived;
        if (!f.broken && f.msgClass == QLatin1String("response")) {
            ++m_tcpResponses;
            const auto it = m_openCommands.find(key(f.msgType, f.msgCount));
            if (it != m_openCommands.end()) {
                f.pair = it.value();
                f.ms = f.clockMs - m_frames[static_cast<std::size_t>(f.pair)].clockMs;
                Frame& cmd = m_frames[static_cast<std::size_t>(f.pair)];
                cmd.pair = index;
                cmd.status = f.status;
                cmd.ms = f.ms;
                m_openCommands.erase(it);
            }
        }
    }
    m_frames.push_back(f);
    if (f.pair >= 0) {
        refreshRow(f.pair);
    }
    appendRow(index);
}

void PsTrafficView::onTimedOut(const QString& msgType, int msgCount)
{
    const auto it = m_openCommands.find(key(msgType, msgCount));
    if (it == m_openCommands.end()) {
        return;
    }
    const int index = it.value();
    m_openCommands.erase(it);
    ++m_tcpTimeouts;
    m_frames[static_cast<std::size_t>(index)].status = QStringLiteral("нет ответа");
    refreshRow(index);
}

bool PsTrafficView::passes(const Frame& f) const
{
    if (f.udp) {
        return m_showUdp->isChecked();
    }
    if (!m_showTcp->isChecked()) {
        return false;
    }
    return m_showKeepAlive->isChecked() || f.msgType != QLatin1String(ps::msg::KeepAlive.data());
}

void PsTrafficView::appendRow(int index)
{
    if (!passes(m_frames[static_cast<std::size_t>(index)])) {
        return;
    }
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    for (int c = 0; c < ColCountAll; ++c) {
        m_table->setItem(row, c, new QTableWidgetItem);
    }
    m_table->item(row, ColTime)->setData(Qt::UserRole, index);
    refreshRow(index);
    if (m_follow->isChecked()) {
        m_table->scrollToBottom();
    }
}

void PsTrafficView::refreshRow(int index)
{
    // Строки добавляются по порядку кадров: ищем с конца.
    int row = -1;
    for (int r = m_table->rowCount() - 1; r >= 0; --r) {
        const int i = m_table->item(r, ColTime)->data(Qt::UserRole).toInt();
        if (i == index) {
            row = r;
            break;
        }
        if (i < index) {
            break;
        }
    }
    if (row < 0) {
        return;
    }
    const Frame& f = m_frames[static_cast<std::size_t>(index)];
    m_table->item(row, ColTime)->setText(f.at.toString(QStringLiteral("HH:mm:ss.zzz")));
    m_table->item(row, ColChannel)->setText(f.udp ? QStringLiteral("UDP") : QStringLiteral("TCP"));
    m_table->item(row, ColDir)->setText(f.outgoing ? QStringLiteral("→ PS") : QStringLiteral("← PS"));
    m_table->item(row, ColType)->setText(f.broken ? QStringLiteral("—") : f.msgType);
    m_table->item(row, ColCount)->setText(f.msgCount >= 0 ? QString::number(f.msgCount) : QString());
    m_table->item(row, ColClass)->setText(f.msgClass);
    m_table->item(row, ColStatus)->setText(f.outgoing && f.status.isEmpty() ? QStringLiteral("ждём ответ…") : f.status);
    m_table->item(row, ColMs)->setText(f.ms >= 0 ? QString::number(f.ms) : QString());
    m_table->item(row, ColBytes)->setText(QString::number(f.line.toUtf8().size()));
}

void PsTrafficView::rebuildTable()
{
    m_table->setRowCount(0);
    for (int i = 0; i < static_cast<int>(m_frames.size()); ++i) {
        appendRow(i);
    }
}

int PsTrafficView::visibleRows() const
{
    return m_table->rowCount();
}

void PsTrafficView::showFrame(int index)
{
    const Frame& f = m_frames[static_cast<std::size_t>(index)];
    const Frame* cmd = f.outgoing ? &f : (f.pair >= 0 ? &m_frames[static_cast<std::size_t>(f.pair)] : nullptr);
    const Frame* rsp = !f.outgoing ? &f : (f.pair >= 0 ? &m_frames[static_cast<std::size_t>(f.pair)] : nullptr);
    const auto title = [](const QString& what, const Frame* x) {
        return x ? QStringLiteral("%1 — %2, %3").arg(what, x->udp ? QStringLiteral("UDP") : QStringLiteral("TCP"),
                                                     x->at.toString(QStringLiteral("HH:mm:ss.zzz")))
                 : what;
    };
    m_sentTitle->setText(title(QStringLiteral("Отправлено"), cmd));
    m_receivedTitle->setText(title(QStringLiteral("Получено"), rsp));
    m_sent->setPlainText(cmd ? pretty(cmd->line) : QString());
    m_sent->setPlaceholderText(f.udp ? QStringLiteral("телеметрия идёт без запроса") : QString());
    QString received = rsp ? pretty(rsp->line) : QString();
    if (rsp && rsp->ms >= 0) {
        received += QStringLiteral("\n\nответ через %1 мс").arg(rsp->ms);
    }
    m_received->setPlainText(received);
    m_received->setPlaceholderText(cmd && cmd->status == QLatin1String("нет ответа") ? QStringLiteral("ответа нет")
                                                                                      : QStringLiteral("ждём ответ…"));
}

void PsTrafficView::refreshCounters()
{
    m_counters->setText(QStringLiteral("TCP: отправлено %1, получено %2 (ответов %3), без ответа %4   ·   "
                                       "UDP: принято %5, потеряно %6, отброшено %7")
                            .arg(m_tcpSent)
                            .arg(m_tcpReceived)
                            .arg(m_tcpResponses)
                            .arg(m_tcpTimeouts)
                            .arg(m_link->telemetryPackets())
                            .arg(m_link->telemetryLost())
                            .arg(m_link->telemetryRejected()));
}

void PsTrafficView::clear()
{
    m_frames.clear();
    m_openCommands.clear();
    m_table->setRowCount(0);
    m_sent->clear();
    m_received->clear();
    m_tcpSent = m_tcpReceived = m_tcpResponses = m_tcpTimeouts = m_udpReceived = 0;
    refreshCounters();
}

bool PsTrafficView::saveLog(const QString& path, QString* error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "# Обмен с PS " << m_link->host() << ':' << m_link->port() << ", UDP " << m_link->telemetryPort() << '\n';
    out << "# время\tканал\tнаправление\tстрока\n";
    for (const Frame& f : m_frames) {
        out << f.at.toString(Qt::ISODateWithMs) << '\t' << (f.udp ? "UDP" : "TCP") << '\t'
            << (f.outgoing ? "->" : "<-") << '\t' << f.line << '\n';
    }
    return file.error() == QFileDevice::NoError;
}
