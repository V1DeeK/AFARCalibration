#include "CodeMatrixTab.h"

#include "S21PlotWidget.h"
#include "Theme.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QString statusText(int status)
{
    switch (status) {
    case 1:
        return QStringLiteral("измеряется");
    case 2:
        return QStringLiteral("готово");
    case 3:
        return QStringLiteral("повтор");
    case 4:
        return QStringLiteral("ошибка");
    case -1:
        return QStringLiteral("вне оси");
    default:
        return QStringLiteral("ожидает");
    }
}

QString statusStyle(int status)
{
    return matrixCellStyle(status, currentAppTheme());
}

}  // namespace

CodeMatrixTab::CodeMatrixTab(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);

    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(QStringLiteral("Канал:"), this));
    m_channel = new QComboBox(this);
    top->addWidget(m_channel);
    top->addWidget(new QLabel(QStringLiteral("Att:"), this));
    m_att = new QComboBox(this);
    top->addWidget(m_att);
    top->addStretch(1);
    m_retrySelected = new QPushButton(QStringLiteral("Повтор выбранной"), this);
    m_retrySelected->setObjectName(QStringLiteral("btnRetry"));
    m_retryRow = new QPushButton(QStringLiteral("Повтор строки"), this);
    m_retryRow->setObjectName(QStringLiteral("btnRetry"));
    top->addWidget(m_retrySelected);
    top->addWidget(m_retryRow);
    root->addLayout(top);

    m_legend = new QLabel(this);
    updateLegend();
    root->addWidget(m_legend);

    auto* gridHost = new QWidget(this);
    m_grid = new QGridLayout(gridHost);
    m_grid->setSpacing(2);
    m_cells.reserve(64);
    for (int phase = 0; phase < 64; ++phase) {
        auto* btn = new QPushButton(QString::number(phase), gridHost);
        btn->setFixedSize(44, 36);
        btn->setProperty("phase", phase);
        btn->setProperty("status", 0);
        btn->setProperty("attempt", 0);
        btn->setProperty("overload", 0);
        // До первого snapshot считаем ось полной 0..63.
        btn->setProperty("inAxis", true);
        btn->setStyleSheet(statusStyle(0));
        btn->setToolTip(QStringLiteral("Фаза %1 — ожидает").arg(phase));
        connect(btn, &QPushButton::clicked, this, &CodeMatrixTab::onCellClicked);
        m_grid->addWidget(btn, phase / 8, phase % 8);
        m_cells.push_back(btn);
    }
    root->addWidget(gridHost, 1);

    m_detail = new QLabel(
        QStringLiteral("Выберите ячейку: время/попытки/флаги и мини-S21 появятся после записи слота."),
        this);
    m_detail->setWordWrap(true);
    root->addWidget(m_detail);
    m_miniPlot = new S21PlotWidget(this);
    m_miniPlot->setMinimumHeight(140);
    root->addWidget(m_miniPlot);

    connect(m_channel, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &CodeMatrixTab::onSelectionEdited);
    connect(m_att, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &CodeMatrixTab::onSelectionEdited);
    connect(m_retrySelected, &QPushButton::clicked, this, &CodeMatrixTab::onRetrySelected);
    connect(m_retryRow, &QPushButton::clicked, this, &CodeMatrixTab::onRetryRow);

    setChannelAttChoices({1}, {0});
}

void CodeMatrixTab::updateLegend()
{
    m_legend->setText(
        QStringLiteral("Статусы: ожидает · измеряется · готово · повтор · ошибка "
                       "(цвет не единственный носитель — подписи в подсказке). "
                       "Повтор — только из Ready/Paused через оркестратор."));
}

void CodeMatrixTab::setChannelAttChoices(const QVector<int>& channels,
                                         const QVector<int>& attCodes)
{
    const int ch = selectedChannel();
    const int att = selectedAttCode();
    m_channel->blockSignals(true);
    m_att->blockSignals(true);
    m_channel->clear();
    m_att->clear();
    for (int c : channels) {
        m_channel->addItem(QString::number(c), c);
    }
    for (int a : attCodes) {
        m_att->addItem(QString::number(a), a);
    }
    if (m_channel->count() == 0) {
        m_channel->addItem(QStringLiteral("1"), 1);
    }
    if (m_att->count() == 0) {
        m_att->addItem(QStringLiteral("0"), 0);
    }
    const int chIdx = m_channel->findData(ch);
    const int attIdx = m_att->findData(att);
    m_channel->setCurrentIndex(chIdx >= 0 ? chIdx : 0);
    m_att->setCurrentIndex(attIdx >= 0 ? attIdx : 0);
    m_channel->blockSignals(false);
    m_att->blockSignals(false);
}

void CodeMatrixTab::applySnapshot(int channel,
                                  int attCode,
                                  int measuringPhase,
                                  const QVector<int>& statuses,
                                  const QVector<int>& attempts,
                                  const QVector<int>& overloadFlags)
{
    Q_UNUSED(measuringPhase);
    if (channel != selectedChannel() || attCode != selectedAttCode()) {
        return;
    }
    const int n = qMin(64, statuses.size());
    for (int i = 0; i < n; ++i) {
        auto* btn = m_cells[i];
        const int st = statuses[i];
        const int attempt = i < attempts.size() ? attempts[i] : 0;
        const int ov = i < overloadFlags.size() ? overloadFlags[i] : 0;
        const bool inAxis = st >= 0;
        btn->setProperty("status", st);
        btn->setProperty("attempt", attempt);
        btn->setProperty("overload", ov);
        btn->setProperty("inAxis", inAxis);
        btn->setEnabled(inAxis);
        btn->setStyleSheet(statusStyle(st));
        btn->setToolTip(QStringLiteral("Фаза %1 — %2; попыток: %3; перегрузка: %4")
                            .arg(i)
                            .arg(statusText(st))
                            .arg(attempt)
                            .arg(ov ? QStringLiteral("да") : QStringLiteral("нет")));
        btn->setText(QStringLiteral("%1\n%2").arg(i).arg(statusText(st).left(3)));
    }
}

int CodeMatrixTab::selectedChannel() const
{
    return m_channel->currentData().toInt();
}

int CodeMatrixTab::selectedAttCode() const
{
    return m_att->currentData().toInt();
}

int CodeMatrixTab::selectedPhase() const
{
    return m_selectedPhase;
}

void CodeMatrixTab::onSelectionEdited()
{
    emit selectionChanged(selectedChannel(), selectedAttCode());
}

void CodeMatrixTab::updateDetailLabel(int status, int attempt, int overload)
{
    const QString timeText = m_slotUtc.isEmpty() ? QStringLiteral("время не записано")
                                                 : m_slotUtc;
    m_detail->setText(
        QStringLiteral("Фаза %1 · время: %2 · статус: %3 · попыток: %4 · перегрузка: %5")
            .arg(m_selectedPhase)
            .arg(timeText)
            .arg(statusText(status))
            .arg(attempt)
            .arg(overload ? QStringLiteral("да") : QStringLiteral("нет")));
}

void CodeMatrixTab::onCellClicked()
{
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn) {
        return;
    }
    const int phase = btn->property("phase").toInt();
    const int st = btn->property("status").toInt();
    const int attempt = btn->property("attempt").toInt();
    const int ov = btn->property("overload").toInt();
    m_selectedPhase = phase;
    m_slotUtc.clear();
    updateDetailLabel(st, attempt, ov);
    emit cellInspectRequested(selectedChannel(), selectedAttCode(), phase);
}

void CodeMatrixTab::setSlotRecordedUtc(int channel,
                                       int attCode,
                                       int phase,
                                       const QString& timestampUtc)
{
    if (channel != selectedChannel() || attCode != selectedAttCode()
        || phase != m_selectedPhase) {
        return;
    }
    m_slotUtc = timestampUtc;
    if (phase < 0 || phase >= m_cells.size()) {
        return;
    }
    auto* btn = m_cells[phase];
    updateDetailLabel(btn->property("status").toInt(), btn->property("attempt").toInt(),
                      btn->property("overload").toInt());
}

void CodeMatrixTab::setCellSweepCurves(const QVector<double>& freqGhz,
                                       const QVector<double>& magDb,
                                       const QVector<double>& phaseUnwrapDeg)
{
    m_miniPlot->setCurves(freqGhz, magDb, phaseUnwrapDeg);
}

void CodeMatrixTab::refreshTheme()
{
    for (auto* btn : m_cells) {
        btn->setStyleSheet(statusStyle(btn->property("status").toInt()));
    }
    if (m_miniPlot) {
        m_miniPlot->update();
    }
}

void CodeMatrixTab::onRetrySelected()
{
    QVector<int> phases;
    phases.push_back(m_selectedPhase);
    emit remeasureRequested(selectedChannel(), selectedAttCode(), phases);
}

void CodeMatrixTab::onRetryRow()
{
    const int row = m_selectedPhase / 8;
    QVector<int> phases;
    phases.reserve(8);
    for (int col = 0; col < 8; ++col) {
        const int phase = row * 8 + col;
        if (phase < 0 || phase >= m_cells.size()) {
            continue;
        }
        // Только фазы, реально присутствующие в оси (полная 0..63 или урезанная).
        if (!m_cells[phase]->property("inAxis").toBool()) {
            continue;
        }
        phases.push_back(phase);
    }
    if (phases.isEmpty()) {
        return;
    }
    emit remeasureRequested(selectedChannel(), selectedAttCode(), phases);
}
