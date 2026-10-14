#include "PsServiceCheckPage.h"

#include "PsServiceCheckRunner.h"
#include "UiStyle.h"

#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ps = afar::ps;

namespace {

enum Col { ColNo, ColPhase, ColTitle, ColCmd, ColExpect, ColGot, ColVerdict, ColMs, ColCount };

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

}  // namespace

PsServiceCheckPage::PsServiceCheckPage(PsLink* link, QWidget* parent)
    : QWidget(parent)
    , m_runner(new PsServiceCheckRunner(link, this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::kPageMargin, ui::kPageMargin, ui::kPageMargin, ui::kPageMargin);
    layout->setSpacing(ui::kGap);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(ui::kInnerGap);
    m_start = new QPushButton(QStringLiteral("Запустить проверку"), this);
    m_start->setObjectName(QStringLiteral("psCheckStart"));
    m_stop = new QPushButton(QStringLiteral("Остановить"), this);
    m_stop->setObjectName(QStringLiteral("psCheckStop"));
    m_save = new QPushButton(QStringLiteral("Сохранить отчёт…"), this);
    m_save->setObjectName(QStringLiteral("psCheckSave"));
    ui::setButtonRole(m_start, ui::ButtonRole::Primary);
    ui::setButtonRole(m_stop, ui::ButtonRole::Danger);
    ui::setButtonRole(m_save, ui::ButtonRole::Secondary);
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    buttons->addStretch(1);
    buttons->addWidget(m_save);

    auto* hint = ui::hintLabel(QStringLiteral(
        "Проверка по контракту ps-json-protocol.md: PS переходит IDLE → SERVICE → IDLE, "
        "выполняет все сервисные команды с обратным чтением и отклоняет неверные. "
        "В конце коды решётки — безопасные 63/0. Перед запуском снимите изделие с антенн."), this);
    m_summary = new QLabel(this);
    m_summary->setObjectName(QStringLiteral("psCheckSummary"));
    m_summary->setProperty("role", QStringLiteral("status"));
    m_summary->setWordWrap(true);

    const auto& steps = m_runner->steps();
    m_table = new QTableWidget(static_cast<int>(steps.size()), ColCount, this);
    m_table->setObjectName(QStringLiteral("psCheckTable"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("№"), QStringLiteral("Этап"), QStringLiteral("Проверка"),
                                        QStringLiteral("Команда"), QStringLiteral("Ожидали"),
                                        QStringLiteral("Получили"), QStringLiteral("Итог"), QStringLiteral("мс")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setMinimumHeight(160);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(ColGot, QHeaderView::Stretch);
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const auto& s = steps[static_cast<std::size_t>(row)];
        const QStringList cells{QString::number(row + 1), qs(s.phase),
                                qs(s.title) + (s.proposal ? QStringLiteral(" · предл.") : QString()),
                                qs(s.msgType), qs(ps::expectedText(s)), QString(),
                                qs(ps::verdictRu(ps::CheckVerdict::NotRun)), QString()};
        for (int col = 0; col < ColCount; ++col) {
            m_table->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    }

    m_request = new QPlainTextEdit(this);
    m_request->setReadOnly(true);
    m_request->setPlaceholderText(QStringLiteral("отправленный payload"));
    m_response = new QPlainTextEdit(this);
    m_response->setReadOnly(true);
    m_response->setPlaceholderText(QStringLiteral("ответ PS"));
    auto* packets = new QSplitter(Qt::Horizontal, this);
    packets->addWidget(m_request);
    packets->addWidget(m_response);
    auto* split = new QSplitter(Qt::Vertical, this);
    split->addWidget(m_table);
    split->addWidget(packets);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);

    layout->addLayout(buttons);
    layout->addWidget(hint);
    layout->addWidget(m_summary);
    layout->addWidget(split, 1);

    connect(m_start, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            m_table->item(row, ColGot)->setText(QString());
            m_table->item(row, ColVerdict)->setText(qs(ps::verdictRu(ps::CheckVerdict::NotRun)));
            m_table->item(row, ColVerdict)->setForeground(QBrush());
            m_table->item(row, ColMs)->setText(QString());
        }
        if (m_runner->start()) {
            emit runningChanged(true);
        } else {
            refreshSummary(QStringLiteral("Нет связи с PS — сначала подключитесь."));
        }
        refreshButtons();
    });
    connect(m_stop, &QPushButton::clicked, this,
            [this] { m_runner->stop(QStringLiteral("остановлено оператором")); });
    connect(m_save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Отчёт приёмочной проверки"), QStringLiteral("ps-service-check.md"),
            QStringLiteral("Markdown (*.md)"));
        if (path.isEmpty()) {
            return;
        }
        QString error;
        if (!saveReport(path, &error)) {
            refreshSummary(error);
        }
    });
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row, int, int, int) { showPackets(row); });
    connect(m_runner, &PsServiceCheckRunner::stepStarted, this, &PsServiceCheckPage::onStepStarted);
    connect(m_runner, &PsServiceCheckRunner::stepFinished, this, &PsServiceCheckPage::onStepFinished);
    connect(m_runner, &PsServiceCheckRunner::finished, this, &PsServiceCheckPage::onFinished);

    refreshSummary(QStringLiteral("Проверка не запускалась."));
    refreshButtons();
}

void PsServiceCheckPage::setAllowed(bool allowed)
{
    m_allowed = allowed;
    refreshButtons();
}

bool PsServiceCheckPage::isRunning() const
{
    return m_runner->isRunning();
}

bool PsServiceCheckPage::saveReport(const QString& path, QString* error) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Отчёт не сохранён: %1").arg(f.errorString());
        }
        return false;
    }
    f.write(m_runner->reportMarkdown().toUtf8());
    return true;
}

void PsServiceCheckPage::onStepStarted(int index)
{
    m_table->item(index, ColVerdict)->setText(QStringLiteral("ждём ответ…"));
    m_table->scrollToItem(m_table->item(index, ColNo));
    refreshSummary();
}

void PsServiceCheckPage::onStepFinished(int index)
{
    const auto& o = m_runner->outcomes()[static_cast<std::size_t>(index)];
    QString verdict = qs(ps::verdictRu(o.result.verdict));
    if (!o.result.detail.empty()) {
        verdict += QStringLiteral(": ") + qs(o.result.detail);
    }
    auto* item = m_table->item(index, ColVerdict);
    item->setText(verdict);
    item->setForeground(o.result.verdict == ps::CheckVerdict::Pass ? QBrush(ui::okColor()) : QBrush(ui::errorColor()));
    m_table->item(index, ColGot)->setText(o.got);
    m_table->item(index, ColMs)->setText(QString::number(o.ms));
    if (m_table->currentRow() == index) {
        showPackets(index);
    }
    refreshSummary();
}

void PsServiceCheckPage::onFinished(const QString& reason)
{
    refreshSummary(reason);
    refreshButtons();
    emit runningChanged(false);
}

void PsServiceCheckPage::showPackets(int row)
{
    if (row < 0 || row >= static_cast<int>(m_runner->outcomes().size())) {
        return;
    }
    const auto& o = m_runner->outcomes()[static_cast<std::size_t>(row)];
    m_request->setPlainText(o.request);
    m_response->setPlainText(o.response);
}

void PsServiceCheckPage::refreshSummary(const QString& tail)
{
    int pass = 0;
    int fail = 0;
    for (const auto& o : m_runner->outcomes()) {
        if (o.result.verdict == ps::CheckVerdict::Pass) {
            ++pass;
        } else if (o.result.verdict != ps::CheckVerdict::NotRun) {
            ++fail;
        }
    }
    QString text = QStringLiteral("Шагов %1: пройдено %2, не пройдено %3.")
                       .arg(m_runner->steps().size())
                       .arg(pass)
                       .arg(fail);
    if (m_runner->isRunning()) {
        text += QStringLiteral(" Идёт шаг %1.").arg(m_runner->currentStep() + 1);
    } else if (!tail.isEmpty()) {
        text += QLatin1Char(' ') + tail + QLatin1Char('.');
    }
    m_summary->setText(text);
    ui::setTone(m_summary, fail > 0 ? ui::Tone::Error
                           : m_runner->isRunning() ? ui::Tone::Info
                           : pass > 0 && pass == static_cast<int>(m_runner->steps().size()) ? ui::Tone::Ok
                                                                                           : ui::Tone::Neutral);
}

void PsServiceCheckPage::refreshButtons()
{
    const bool running = m_runner->isRunning();
    m_start->setEnabled(m_allowed && !running);
    m_stop->setEnabled(running);
    m_save->setEnabled(!running);
}
