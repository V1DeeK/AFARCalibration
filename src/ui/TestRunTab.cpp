#include "TestRunTab.h"

#include "PsExchangeView.h"
#include "PsSession.h"
#include "StressTestPanel.h"
#include "UiStyle.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

QString pretty(const QString& line)
{
    const json j = json::parse(line.toStdString(), nullptr, false);
    return j.is_discarded() ? line : QString::fromStdString(j.dump(2));
}

}  // namespace

TestRunTab::TestRunTab(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ui::kPageMargin, ui::kPageMargin, ui::kPageMargin, ui::kPageMargin);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("testRunTabs"));
    tabs->addTab(buildStressPage(), QStringLiteral("Стресс-тест TCP"));
    tabs->addTab(buildUdpPage(), QStringLiteral("UDP-телеметрия и узлы"));
    tabs->addTab(buildRs485Page(), QStringLiteral("RS-485"));
    auto* journal = new PsExchangeView(m_session, tabs);
    journal->table()->setObjectName(QStringLiteral("testRunJournal"));
    tabs->addTab(journal, QStringLiteral("Журнал"));
    root->addWidget(tabs);

    connect(m_session, &PsSession::controlsChanged, this, &TestRunTab::refreshEnabled);
    connect(m_session, &PsSession::modeChanged, this, &TestRunTab::refreshEnabled);
    connect(m_session, &PsSession::telemetryChanged, this, &TestRunTab::refreshUdp);
    refreshEnabled();
    refreshUdp();
}

QWidget* TestRunTab::buildStressPage()
{
    // Безопасный по умолчанию набор: запросы, которые не меняют настроек платы.
    const QVector<StressTestPanel::Command> commands{
        {std::string(ps::msg::GetState), json::object()},
        {std::string(ps::msg::GetSffParameters), json::object()},
        {std::string(ps::msg::GetPhaseAtten), json::object()},
        {std::string(ps::msg::GetAdc), json::object()},
        {std::string(ps::msg::GetBeam), json::object()},
        {std::string(ps::msg::GetLo), ps::loGetPayload(ps::kLoMixer)},
        {std::string(ps::msg::GetLo), ps::loGetPayload(ps::kLoCalibration)},
        {std::string(ps::msg::GetMixer), json::object()},
        {std::string(ps::msg::GetVideo), json::object()},
    };
    auto* split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    m_stress = new StressTestPanel(m_session, commands, split);

    auto* packets = new QWidget(split);
    auto* layout = new QVBoxLayout(packets);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(QStringLiteral("Кликните строку таблицы, чтобы увидеть её последний проблемный обмен"),
                             packets);
    auto* pair = new QHBoxLayout;
    auto* request = new QPlainTextEdit(packets);
    auto* response = new QPlainTextEdit(packets);
    for (QPlainTextEdit* e : {request, response}) {
        e->setReadOnly(true);
        e->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        pair->addWidget(e, 1);
    }
    request->setPlaceholderText(QStringLiteral("отправлено"));
    response->setPlaceholderText(QStringLiteral("получено"));
    layout->addWidget(title);
    layout->addLayout(pair, 1);
    split->addWidget(m_stress);
    split->addWidget(packets);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);

    connect(m_stress, &StressTestPanel::exchangeChosen, this,
            [title, request, response](const QString& text, const QString& req, const QString& resp) {
                title->setText(text);
                request->setPlainText(pretty(req));
                response->setPlainText(resp.isEmpty() ? QString() : pretty(resp));
                response->setPlaceholderText(QStringLiteral("ответа нет"));
            });
    return split;
}

QWidget* TestRunTab::buildUdpPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kGap);
    auto* hint = ui::hintLabel(QStringLiteral("PS шлёт телеметрию раз в секунду (контракт §5). Проверка "
                                              "считает принятые, потерянные по msg_count и отброшенные "
                                              "датаграммы за выбранное время. Ниже — состояние узлов платы "
                                              "из последней телеметрии: ПЛИС (PL), RS-485, АЦП и др."),
                               page);
    auto* row = new QHBoxLayout;
    row->setSpacing(ui::kInnerGap);
    m_udpSeconds = new QSpinBox(page);
    m_udpSeconds->setObjectName(QStringLiteral("udpCheckSeconds"));
    m_udpSeconds->setRange(2, 3600);
    m_udpSeconds->setValue(10);
    m_udpSeconds->setSuffix(QStringLiteral(" с"));
    m_udpSeconds->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_udpStart = new QPushButton(QStringLiteral("Проверить поток"), page);
    m_udpStart->setObjectName(QStringLiteral("udpCheckStart"));
    ui::setButtonRole(m_udpStart, ui::ButtonRole::Primary);
    row->addWidget(new QLabel(QStringLiteral("Длительность"), page));
    row->addWidget(m_udpSeconds);
    row->addWidget(m_udpStart);
    row->addStretch(1);
    m_udpCounters = new QLabel(page);
    m_udpCounters->setObjectName(QStringLiteral("udpCounters"));
    m_udpVerdict = new QLabel(page);
    m_udpVerdict->setObjectName(QStringLiteral("udpVerdict"));
    m_udpVerdict->setProperty("role", QStringLiteral("status"));
    m_udpVerdict->setWordWrap(true);
    m_devices = new QTableWidget(0, 2, page);
    m_devices->setObjectName(QStringLiteral("udpDevices"));
    m_devices->setHorizontalHeaderLabels({QStringLiteral("Узел"), QStringLiteral("Самопроверка")});
    m_devices->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_devices->verticalHeader()->setVisible(false);
    m_devices->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_devices->setAlternatingRowColors(true);
    m_devices->setShowGrid(false);
    m_devices->setMinimumHeight(140);
    layout->addWidget(hint);
    layout->addLayout(row);
    layout->addWidget(m_udpCounters);
    layout->addWidget(m_udpVerdict);
    layout->addWidget(m_devices, 1);

    m_udpTimer = new QTimer(this);
    m_udpTimer->setInterval(250);
    connect(m_udpTimer, &QTimer::timeout, this, [this] {
        refreshUdp();
        if (m_udpClock.elapsed() >= qint64(m_udpSeconds->value()) * 1000) {
            finishUdpCheck();
        }
    });
    connect(m_udpStart, &QPushButton::clicked, this, [this] { startUdpCheck(m_udpSeconds->value()); });
    return page;
}

QWidget* TestRunTab::buildRs485Page()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kGap);
    m_rawHex = new QLineEdit(page);
    m_rawHex->setObjectName(QStringLiteral("psRawHex"));
    m_rawHex->setPlaceholderText(QStringLiteral("например 0101020F1E…"));
    m_rawSend = new QPushButton(QStringLiteral("Передать в линию"), page);
    m_rawSend->setObjectName(QStringLiteral("psRawSend"));
    ui::setButtonRole(m_rawSend, ui::ButtonRole::Primary);
    m_rawResult = ui::hintLabel(QStringLiteral("Байты уходят в линию RS-485 без изменений, включая CRC16 "
                                               "(rs485_send, только в сервисном режиме). Ответ устройства PS "
                                               "пока не возвращает — смотрите статус ответа и журнал."),
                                page);
    m_rawResult->setObjectName(QStringLiteral("psRawResult"));
    auto* row = new QHBoxLayout;
    row->addWidget(m_rawHex, 1);
    row->addWidget(m_rawSend);
    layout->addLayout(row);
    layout->addWidget(m_rawResult);
    layout->addStretch(1);
    connect(m_rawSend, &QPushButton::clicked, this, [this] {
        const QString hex = m_rawHex->text().remove(QLatin1Char(' ')).toUpper();
        if (!ps::hexBytesValid(hex.toStdString())) {
            m_rawResult->setText(QStringLiteral("HEX-строка должна быть чётной длины и только из 0–9, A–F"));
            return;
        }
        const int count = m_session->send(ps::msg::Rs485Send, json{{"data", hex.toStdString()}});
        m_rawResult->setText(count < 0 ? m_session->lastError()
                                       : QStringLiteral("rs485_send №%1 отправлена").arg(count));
    });
    return page;
}

void TestRunTab::startUdpCheck(int seconds)
{
    m_udpSeconds->setValue(seconds);
    const PsLink* link = m_session->link();
    m_udpStartReceived = link->telemetryPackets();
    m_udpStartLost = link->telemetryLost();
    m_udpStartRejected = link->telemetryRejected();
    m_udpClock.start();
    m_udpTimer->start();
    m_udpStart->setEnabled(false);
    m_udpVerdict->setText(QStringLiteral("Идёт проверка потока %1 с…").arg(seconds));
    ui::setTone(m_udpVerdict, ui::Tone::Info);
}

bool TestRunTab::udpCheckRunning() const
{
    return m_udpTimer->isActive();
}

void TestRunTab::finishUdpCheck()
{
    m_udpTimer->stop();
    const PsLink* link = m_session->link();
    const quint64 received = link->telemetryPackets() - m_udpStartReceived;
    const quint64 lost = link->telemetryLost() - m_udpStartLost;
    const quint64 rejected = link->telemetryRejected() - m_udpStartRejected;
    const double secs = double(m_udpClock.elapsed()) / 1000.0;
    // Период PS 1 с: за N секунд ждём не меньше N − 1 датаграмм.
    const bool ok = received + 1 >= quint64(secs) && lost == 0 && rejected == 0;
    m_udpVerdict->setText(QStringLiteral("%1: за %2 с принято %3, потеряно %4, отброшено %5")
                              .arg(ok ? QStringLiteral("Норма") : QStringLiteral("Не норма"))
                              .arg(secs, 0, 'f', 1)
                              .arg(received)
                              .arg(lost)
                              .arg(rejected));
    ui::setTone(m_udpVerdict, ok ? ui::Tone::Ok : ui::Tone::Error);
    refreshEnabled();
}

void TestRunTab::refreshUdp()
{
    const PsLink* link = m_session->link();
    m_udpCounters->setText(QStringLiteral("Всего с подключения: принято %1, потеряно %2, отброшено %3")
                               .arg(link->telemetryPackets())
                               .arg(link->telemetryLost())
                               .arg(link->telemetryRejected()));
    const auto& t = m_session->telemetry();
    if (!t) {
        return;
    }
    m_devices->setRowCount(static_cast<int>(t->devices.size()));
    int row = 0;
    for (const auto& [id, health] : t->devices) {
        const std::string_view h = ps::toString(health);
        auto* name = new QTableWidgetItem(QStringLiteral("%1 (%2)").arg(
            QString::fromStdString(ps::selfTestRu(id)), QString::fromStdString(id)));
        auto* value = new QTableWidgetItem(QString::fromUtf8(h.data(), static_cast<qsizetype>(h.size())));
        if (health == ps::DeviceHealth::Fail) {
            value->setForeground(QBrush(ui::errorColor()));
        }
        m_devices->setItem(row, 0, name);
        m_devices->setItem(row, 1, value);
        ++row;
    }
}

void TestRunTab::refreshEnabled()
{
    const bool ready = m_session->manualAllowed();
    m_udpStart->setEnabled(m_session->link()->isReady() && !m_udpTimer->isActive());
    m_rawSend->setEnabled(ready && m_session->modeKnown() && m_session->mode() == ps::Mode::Service);
}
