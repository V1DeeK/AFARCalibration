#include "PsExchangeView.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace ps = afar::ps;

namespace {

constexpr int kRows = 2000;

}  // namespace

PsExchangeView::PsExchangeView(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_table = new QTableWidget(0, ColCount, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Время"), QStringLiteral("Канал"),
                                        QStringLiteral("Направление"), QStringLiteral("msg_type"),
                                        QStringLiteral("msg_count"), QStringLiteral("status"),
                                        QStringLiteral("JSON")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColJson, QHeaderView::Stretch);
    m_table->setMinimumHeight(110);

    auto* row = new QHBoxLayout;
    m_showUdp = new QCheckBox(QStringLiteral("Показывать датаграммы телеметрии (UDP)"), this);
    m_showUdp->setChecked(true);
    auto* clear = new QPushButton(QStringLiteral("Очистить журнал"), this);
    row->addWidget(m_showUdp);
    row->addStretch(1);
    row->addWidget(clear);
    layout->addWidget(m_table, 1);
    layout->addLayout(row);

    connect(m_showUdp, &QCheckBox::toggled, this, &PsExchangeView::rebuild);
    connect(clear, &QPushButton::clicked, m_session, &PsSession::clearLog);
    connect(m_session, &PsSession::frameAppended, this, &PsExchangeView::append);
    connect(m_session, &PsSession::logCleared, this, &PsExchangeView::rebuild);
    rebuild();
}

void PsExchangeView::rebuild()
{
    m_table->setRowCount(0);
    const auto& log = m_session->log();
    const qsizetype from = std::max<qsizetype>(0, log.size() - kRows);
    for (qsizetype i = from; i < log.size(); ++i) {
        append(log.at(i));
    }
}

void PsExchangeView::append(const PsSession::LogLine& line)
{
    if (line.channel == QLatin1String("UDP") && !m_showUdp->isChecked()) {
        return;
    }
    ps::Envelope env;
    std::string error;
    const bool parsed = ps::decodeLine(line.line.toStdString(), env, &error);
    QString status;
    if (parsed) {
        const auto it = env.payload.find("status");
        if (it != env.payload.end() && it->is_string()) {
            status = QString::fromStdString(it->get<std::string>());
        }
    }
    // Старые строки уходят, чтобы таблица не росла без предела.
    while (m_table->rowCount() >= kRows) {
        m_table->removeRow(0);
    }
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    const QStringList cells{
        line.time.toString(QStringLiteral("HH:mm:ss.zzz")),
        line.channel,
        line.outgoing ? QStringLiteral("→ ушло") : QStringLiteral("← пришло"),
        parsed ? QString::fromStdString(env.msg_type) : QStringLiteral("не разобрано"),
        parsed ? QString::number(env.msg_count) : QString(),
        parsed ? status : QString::fromStdString(error),
        line.line,
    };
    const bool bad = !parsed || (!status.isEmpty() && status != QLatin1String("OK"));
    for (int col = 0; col < ColCount; ++col) {
        auto* item = new QTableWidgetItem(cells[col]);
        if (col == ColJson) {
            item->setToolTip(line.line);
        }
        if (bad) {
            item->setForeground(QBrush(Qt::red));
        }
        m_table->setItem(row, col, item);
    }
    m_table->scrollToBottom();
}
