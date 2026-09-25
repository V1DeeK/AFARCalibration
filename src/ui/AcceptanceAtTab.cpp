#include "AcceptanceAtTab.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {
constexpr auto kOrg = "at_acceptance";
}

AcceptanceAtTab::AcceptanceAtTab(QWidget* parent)
    : QWidget(parent)
{
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto* inner = new QWidget(scroll);
    auto* root = new QVBoxLayout(inner);

    auto* title = new QLabel(QStringLiteral("Приёмка AT — оболочки чеклистов"), inner);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    auto* disclaimer = new QLabel(
        QStringLiteral(
            "Статусы хранятся локально (QSettings). Это операторский чеклист, "
            "а не замена стендового протокола и не аттестованная метрология."),
        inner);
    disclaimer->setWordWrap(true);
    disclaimer->setObjectName(QStringLiteral("hintLabel"));
    root->addWidget(title);
    root->addWidget(disclaimer);

    // AT-12
    {
        auto* box = new QGroupBox(QStringLiteral("AT-12 — длительный прогон (24 ч)"), inner);
        auto* lay = new QVBoxLayout(box);
        lay->addWidget(new QLabel(
            QStringLiteral(
                "Старт/стоп таймера чеклиста. Фактический 24-часовой прогон и ротация "
                "журнала выполняются на стенде — эта панель только фиксирует намерение."),
            box));
        m_at12Status = new QLabel(box);
        m_at12Status->setWordWrap(true);
        auto* row = new QHBoxLayout();
        m_at12Start = new QPushButton(QStringLiteral("Старт таймера чеклиста"), box);
        m_at12Stop = new QPushButton(QStringLiteral("Стоп"), box);
        row->addWidget(m_at12Start);
        row->addWidget(m_at12Stop);
        row->addStretch(1);
        auto* rssRow = new QHBoxLayout();
        m_at12FixRss = new QPushButton(QStringLiteral("Зафиксировать RSS (оценка)"), box);
        m_at12RssMb = new QLineEdit(box);
        m_at12RssMb->setPlaceholderText(QStringLiteral("MB (оценка оператора)"));
        m_at12RssMb->setMaximumWidth(140);
        rssRow->addWidget(m_at12FixRss);
        rssRow->addWidget(m_at12RssMb);
        rssRow->addStretch(1);
        m_at12RssNote = new QLabel(box);
        m_at12RssNote->setWordWrap(true);
        m_at12RssNote->setObjectName(QStringLiteral("hintLabel"));
        m_at12MemoryNote = new QCheckBox(
            QStringLiteral("Заметка: следить за памятью / ротацией журнала"), box);
        m_at12NotProtocol = new QCheckBox(
            QStringLiteral("Понимаю: не заменяет стендовый протокол AT-12"), box);
        lay->addWidget(m_at12Status);
        lay->addLayout(row);
        lay->addLayout(rssRow);
        lay->addWidget(m_at12RssNote);
        lay->addWidget(m_at12MemoryNote);
        lay->addWidget(m_at12NotProtocol);
        connect(m_at12Start, &QPushButton::clicked, this, &AcceptanceAtTab::onAt12Start);
        connect(m_at12Stop, &QPushButton::clicked, this, &AcceptanceAtTab::onAt12Stop);
        connect(m_at12FixRss, &QPushButton::clicked, this, &AcceptanceAtTab::onAt12FixRss);
        connect(m_at12RssMb, &QLineEdit::editingFinished, this, &AcceptanceAtTab::persistAll);
        connect(m_at12MemoryNote, &QCheckBox::toggled, this, &AcceptanceAtTab::persistAll);
        connect(m_at12NotProtocol, &QCheckBox::toggled, this, &AcceptanceAtTab::persistAll);
        root->addWidget(box);
    }

    // AT-13
    {
        auto* box = new QGroupBox(QStringLiteral("AT-13 — THRU + калибровка + измерение"), inner);
        auto* lay = new QFormLayout(box);
        m_at13Thru = new QCheckBox(QStringLiteral("THRU проверен (пороги FR-05 / мастер)"), box);
        m_at13CalId = new QLineEdit(box);
        m_at13CalId->setPlaceholderText(QStringLiteral("vna_calibration_id"));
        m_at13Limits = new QCheckBox(
            QStringLiteral("Пределы утверждены метрологом (см. мастер)"), box);
        m_at13MeasureLink = new QCheckBox(
            QStringLiteral("Ссылка: этап 4 «Измерить сейчас» / серия выполнены"), box);
        lay->addRow(m_at13Thru);
        lay->addRow(QStringLiteral("cal id"), m_at13CalId);
        lay->addRow(m_at13Limits);
        lay->addRow(m_at13MeasureLink);
        connect(m_at13Thru, &QCheckBox::toggled, this, &AcceptanceAtTab::persistAll);
        connect(m_at13CalId, &QLineEdit::editingFinished, this, &AcceptanceAtTab::persistAll);
        connect(m_at13Limits, &QCheckBox::toggled, this, &AcceptanceAtTab::persistAll);
        connect(m_at13MeasureLink, &QCheckBox::toggled, this, &AcceptanceAtTab::persistAll);
        root->addWidget(box);
    }

    // AT-14
    {
        auto* box = new QGroupBox(QStringLiteral("AT-14 — каналы / коды (боевой DUT)"), inner);
        auto* lay = new QVBoxLayout(box);
        m_at14Blocker = new QLabel(
            QStringLiteral(
                "Нужен т. 14 / боевой DUT. Пока протокол контроллера не передан — "
                "чеклист только для планирования; apply/readback на stub недоступны."),
            box);
        m_at14Blocker->setWordWrap(true);
        m_at14Blocker->setObjectName(QStringLiteral("hintLabel"));
        lay->addWidget(m_at14Blocker);
        m_at14Ch1 = new QCheckBox(QStringLiteral("Канал 1"), box);
        m_at14Ch8 = new QCheckBox(QStringLiteral("Канал 8"), box);
        m_at14Ch16 = new QCheckBox(QStringLiteral("Канал 16"), box);
        m_at14Code0 = new QCheckBox(QStringLiteral("Код 0"), box);
        m_at14Code31 = new QCheckBox(QStringLiteral("Код 31"), box);
        m_at14Code63 = new QCheckBox(QStringLiteral("Код 63"), box);
        for (auto* cb : {m_at14Ch1, m_at14Ch8, m_at14Ch16, m_at14Code0, m_at14Code31,
                         m_at14Code63}) {
            lay->addWidget(cb);
            connect(cb, &QCheckBox::toggled, this, &AcceptanceAtTab::persistAll);
        }
        root->addWidget(box);
    }

    root->addStretch(1);
    scroll->setWidget(inner);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    loadFromSettings();
}

void AcceptanceAtTab::onAt12Start()
{
    QSettings s;
    s.beginGroup(QString::fromLatin1(kOrg));
    s.setValue(QStringLiteral("at12/running"), true);
    s.setValue(QStringLiteral("at12/started_utc"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    s.remove(QStringLiteral("at12/stopped_utc"));
    s.endGroup();
    refreshAt12Status();
}

void AcceptanceAtTab::onAt12Stop()
{
    QSettings s;
    s.beginGroup(QString::fromLatin1(kOrg));
    s.setValue(QStringLiteral("at12/running"), false);
    s.setValue(QStringLiteral("at12/stopped_utc"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    s.endGroup();
    refreshAt12Status();
}

void AcceptanceAtTab::onAt12FixRss()
{
    QString mbText = m_at12RssMb->text().trimmed();
#ifdef Q_OS_WIN
    if (mbText.isEmpty()) {
        PROCESS_MEMORY_COUNTERS pmc{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            const auto mb = static_cast<qint64>(pmc.WorkingSetSize / (1024ull * 1024ull));
            mbText = QString::number(mb);
            m_at12RssMb->setText(mbText);
        }
    }
#endif
    const QString stamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString note = mbText.isEmpty()
        ? QStringLiteral("%1 UTC — RSS не указан (оценка оператора)").arg(stamp)
        : QStringLiteral("%1 UTC — RSS ≈ %2 MB (оценка, не 24ч автомат)").arg(stamp, mbText);
    QSettings s;
    s.beginGroup(QString::fromLatin1(kOrg));
    s.setValue(QStringLiteral("at12/rss_note"), note);
    s.setValue(QStringLiteral("at12/rss_mb"), mbText);
    s.endGroup();
    m_at12RssNote->setText(note);
    persistAll();
}

void AcceptanceAtTab::persistAll()
{
    QSettings s;
    s.beginGroup(QString::fromLatin1(kOrg));
    s.setValue(QStringLiteral("at12/memory_note"), m_at12MemoryNote->isChecked());
    s.setValue(QStringLiteral("at12/not_protocol"), m_at12NotProtocol->isChecked());
    s.setValue(QStringLiteral("at12/rss_mb"), m_at12RssMb->text().trimmed());
    s.setValue(QStringLiteral("at13/thru"), m_at13Thru->isChecked());
    s.setValue(QStringLiteral("at13/cal_id"), m_at13CalId->text().trimmed());
    s.setValue(QStringLiteral("at13/limits"), m_at13Limits->isChecked());
    s.setValue(QStringLiteral("at13/measure_link"), m_at13MeasureLink->isChecked());
    s.setValue(QStringLiteral("at14/ch1"), m_at14Ch1->isChecked());
    s.setValue(QStringLiteral("at14/ch8"), m_at14Ch8->isChecked());
    s.setValue(QStringLiteral("at14/ch16"), m_at14Ch16->isChecked());
    s.setValue(QStringLiteral("at14/code0"), m_at14Code0->isChecked());
    s.setValue(QStringLiteral("at14/code31"), m_at14Code31->isChecked());
    s.setValue(QStringLiteral("at14/code63"), m_at14Code63->isChecked());
    s.endGroup();
}

void AcceptanceAtTab::loadFromSettings()
{
    QSettings s;
    s.beginGroup(QString::fromLatin1(kOrg));
    m_at12MemoryNote->setChecked(s.value(QStringLiteral("at12/memory_note")).toBool());
    m_at12NotProtocol->setChecked(s.value(QStringLiteral("at12/not_protocol")).toBool());
    m_at12RssMb->setText(s.value(QStringLiteral("at12/rss_mb")).toString());
    m_at12RssNote->setText(s.value(QStringLiteral("at12/rss_note")).toString());
    if (m_at12RssNote->text().isEmpty()) {
        m_at12RssNote->setText(
            QStringLiteral("RSS ещё не фиксировали. Кнопка пишет оценку с меткой времени "
                           "(не автоматический 24-часовой прогон)."));
    }
    m_at13Thru->setChecked(s.value(QStringLiteral("at13/thru")).toBool());
    m_at13CalId->setText(s.value(QStringLiteral("at13/cal_id")).toString());
    m_at13Limits->setChecked(s.value(QStringLiteral("at13/limits")).toBool());
    m_at13MeasureLink->setChecked(s.value(QStringLiteral("at13/measure_link")).toBool());
    m_at14Ch1->setChecked(s.value(QStringLiteral("at14/ch1")).toBool());
    m_at14Ch8->setChecked(s.value(QStringLiteral("at14/ch8")).toBool());
    m_at14Ch16->setChecked(s.value(QStringLiteral("at14/ch16")).toBool());
    m_at14Code0->setChecked(s.value(QStringLiteral("at14/code0")).toBool());
    m_at14Code31->setChecked(s.value(QStringLiteral("at14/code31")).toBool());
    m_at14Code63->setChecked(s.value(QStringLiteral("at14/code63")).toBool());
    s.endGroup();
    refreshAt12Status();
}

void AcceptanceAtTab::refreshAt12Status()
{
    QSettings s;
    s.beginGroup(QString::fromLatin1(kOrg));
    const bool running = s.value(QStringLiteral("at12/running")).toBool();
    const QString started = s.value(QStringLiteral("at12/started_utc")).toString();
    const QString stopped = s.value(QStringLiteral("at12/stopped_utc")).toString();
    s.endGroup();
    if (running) {
        m_at12Status->setText(
            QStringLiteral("Таймер чеклиста: идёт с %1 (UTC). Не заменяет протокол стенда.")
                .arg(started.isEmpty() ? QStringLiteral("—") : started));
    } else if (!stopped.isEmpty()) {
        m_at12Status->setText(
            QStringLiteral("Таймер чеклиста: остановлен %1 (UTC). Старт был: %2.")
                .arg(stopped, started.isEmpty() ? QStringLiteral("—") : started));
    } else {
        m_at12Status->setText(QStringLiteral("Таймер чеклиста: не запускался."));
    }
}
