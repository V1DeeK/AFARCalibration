#include "ConnectionTab.h"

#include "FullScanPanel.h"
#include "MacResolver.h"
#include "PsJournalDialog.h"
#include "PsPacketPage.h"
#include "PsSession.h"

#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

namespace ps = afar::ps;

namespace {

constexpr int kRecentLines = 300;

/// Плитка мозаики: полупрозрачный фон читается и в светлой, и в тёмной теме.
QLabel* tile(QWidget* parent, const QString& text, const char* tone)
{
    auto* label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignCenter);
    label->setWordWrap(true);
    label->setMinimumHeight(34);
    QString bg = QStringLiteral("rgba(128,128,128,0.18)");
    if (qstrcmp(tone, "ok") == 0) {
        bg = QStringLiteral("rgba(46,160,67,0.28)");
    } else if (qstrcmp(tone, "fail") == 0) {
        bg = QStringLiteral("rgba(220,53,69,0.32)");
    } else if (qstrcmp(tone, "info") == 0) {
        bg = QStringLiteral("rgba(56,132,229,0.22)");
    }
    label->setStyleSheet(QStringLiteral("QLabel { background: %1; border-radius: 4px; padding: 3px 6px; }").arg(bg));
    return label;
}

QString uptimeText(qint64 sec)
{
    return QStringLiteral("%1:%2:%3")
        .arg(sec / 3600)
        .arg((sec / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(sec % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

ConnectionTab::ConnectionTab(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);

    auto* left = new QWidget(split);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    auto* top = new QHBoxLayout;
    top->addWidget(buildConnectionBox(), 1);
    top->addWidget(buildTechBox(), 1);
    leftLayout->addLayout(top);
    leftLayout->addWidget(buildCommandBox(), 1);

    auto* right = new QWidget(split);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(buildJournalBox());
    auto* scanBox = new QGroupBox(QStringLiteral("Полное сканирование"), right);
    auto* scanLayout = new QVBoxLayout(scanBox);
    m_fullScan = new FullScanPanel(m_session, scanBox);
    scanLayout->addWidget(m_fullScan);
    rightLayout->addWidget(scanBox, 1);

    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    root->addWidget(split);

    m_journal = new PsJournalDialog(m_session, this);

    connect(m_session, &PsSession::statusChanged, this, &ConnectionTab::refreshConnection);
    connect(m_session, &PsSession::controlsChanged, this, &ConnectionTab::refreshConnection);
    connect(m_session, &PsSession::modeChanged, this, &ConnectionTab::refreshTech);
    connect(m_session, &PsSession::telemetryChanged, this, &ConnectionTab::refreshTech);
    connect(m_session, &PsSession::hostResolved, m_host, &QLineEdit::setText);
    connect(m_session, &PsSession::message, this, [this](const QString& text) {
        m_message->setText(text);
        ++m_errorCount;
        refreshCounters();
    });
    connect(m_session, &PsSession::frameAppended, this, [this](const PsSession::LogLine& l) {
        if (l.channel == QLatin1String("TCP")) {
            ++(l.outgoing ? m_tcpOutCount : m_tcpInCount);
        }
        ps::Envelope env;
        const bool parsed = ps::decodeLine(l.line.toStdString(), env);
        QString status;
        if (parsed) {
            const auto it = env.payload.find("status");
            if (it != env.payload.end() && it->is_string()) {
                status = QStringLiteral(" %1").arg(QString::fromStdString(it->get<std::string>()));
            }
        }
        m_recent->appendPlainText(QStringLiteral("%1 %2 %3 %4%5")
                                      .arg(l.time.toString(QStringLiteral("HH:mm:ss")), l.channel,
                                           l.outgoing ? QStringLiteral("→") : QStringLiteral("←"),
                                           parsed ? QStringLiteral("%1 №%2")
                                                        .arg(QString::fromStdString(env.msg_type))
                                                        .arg(env.msg_count)
                                                  : QStringLiteral("не разобрано"),
                                           status));
        refreshCounters();
    });
    connect(m_session, &PsSession::logCleared, this, [this] {
        m_recent->clear();
        m_tcpOutCount = 0;
        m_tcpInCount = 0;
        m_errorCount = 0;
        refreshCounters();
    });

    refreshConnection();
    refreshTech();
    refreshCounters();
}

QWidget* ConnectionTab::buildConnectionBox()
{
    auto* box = new QGroupBox(QStringLiteral("Подключение к плате"), this);
    auto* form = new QFormLayout(box);
    m_host = new QLineEdit(m_session->host(), box);
    m_host->setObjectName(QStringLiteral("psHost"));
    m_host->setPlaceholderText(QStringLiteral("IP-адрес PS; пусто — искать по MAC"));
    m_mac = new QLineEdit(m_session->mac(), box);
    m_mac->setObjectName(QStringLiteral("psMac"));
    m_mac->setPlaceholderText(QStringLiteral("AA:BB:CC:DD:EE:FF"));
    m_mac->setToolTip(QStringLiteral("IP платы меняется, MAC постоянный: если по адресу PS не "
                                     "отвечает, плата ищется по MAC в локальной сети"));
    m_port = new QSpinBox(box);
    m_port->setObjectName(QStringLiteral("psPort"));
    m_port->setRange(1, 65535);
    m_port->setValue(m_session->port());
    m_port->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_tlmPort = new QSpinBox(box);
    m_tlmPort->setObjectName(QStringLiteral("psTlmPort"));
    m_tlmPort->setRange(1, 65535);
    m_tlmPort->setValue(m_session->telemetryPort());
    m_tlmPort->setButtonSymbols(QAbstractSpinBox::NoButtons);
    auto* ports = new QHBoxLayout;
    ports->addWidget(m_port, 1);
    ports->addWidget(new QLabel(QStringLiteral("UDP-порт"), box));
    ports->addWidget(m_tlmPort, 1);

    m_linkState = new QLabel(box);
    m_linkState->setObjectName(QStringLiteral("psLinkState"));
    m_linkState->setWordWrap(true);
    m_message = new QLabel(box);
    m_message->setObjectName(QStringLiteral("psMessage"));
    m_message->setWordWrap(true);
    m_message->setStyleSheet(QStringLiteral("color: palette(mid);"));

    m_connect = new QPushButton(QStringLiteral("Подключиться"), box);
    m_connect->setObjectName(QStringLiteral("psConnect"));
    m_findByMac = new QPushButton(QStringLiteral("Найти по MAC"), box);
    m_findByMac->setObjectName(QStringLiteral("psFindByMac"));
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(m_connect, 1);
    buttons->addWidget(m_findByMac);

    form->addRow(QStringLiteral("IP"), m_host);
    form->addRow(QStringLiteral("MAC"), m_mac);
    form->addRow(QStringLiteral("TCP-порт"), ports);
    form->addRow(QStringLiteral("Статус"), m_linkState);
    form->addRow(buttons);
    form->addRow(m_message);

    connect(m_connect, &QPushButton::clicked, this, &ConnectionTab::onConnectClicked);
    connect(m_host, &QLineEdit::returnPressed, this, &ConnectionTab::onConnectClicked);
    connect(m_findByMac, &QPushButton::clicked, this, [this] {
        m_session->findByMac(m_mac->text().trimmed(), static_cast<quint16>(m_port->value()),
                             static_cast<quint16>(m_tlmPort->value()));
    });
    return box;
}

QWidget* ConnectionTab::buildTechBox()
{
    auto* box = new QGroupBox(QStringLiteral("Тех. состояние"), this);
    auto* layout = new QVBoxLayout(box);
    m_techTabs = new QTabWidget(box);
    auto* page = new QWidget(m_techTabs);
    auto* pageLayout = new QVBoxLayout(page);
    m_techMode = new QLabel(page);
    m_techMode->setObjectName(QStringLiteral("psTechMode"));
    m_techMode->setWordWrap(true);
    m_techTiles = new QGridLayout;
    m_techTiles->setSpacing(4);
    m_techValues = new QLabel(page);
    m_techValues->setObjectName(QStringLiteral("psTechValues"));
    m_techValues->setWordWrap(true);
    m_techValues->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pageLayout->addWidget(m_techMode);
    pageLayout->addLayout(m_techTiles);
    pageLayout->addWidget(m_techValues);
    pageLayout->addStretch(1);
    m_techTabs->addTab(page, QStringLiteral("Телеметрия PS"));
    layout->addWidget(m_techTabs);
    return box;
}

QWidget* ConnectionTab::buildJournalBox()
{
    auto* box = new QGroupBox(QStringLiteral("Журнал обмена"), this);
    auto* layout = new QVBoxLayout(box);
    auto* tiles = new QGridLayout;
    tiles->setSpacing(4);
    m_tcpOut = tile(box, QString(), "info");
    m_tcpIn = tile(box, QString(), "info");
    m_udpIn = tile(box, QString(), "info");
    m_udpLost = tile(box, QString(), "info");
    m_errors = tile(box, QString(), "info");
    m_errors->setObjectName(QStringLiteral("psJournalErrors"));
    tiles->addWidget(m_tcpOut, 0, 0);
    tiles->addWidget(m_tcpIn, 0, 1);
    tiles->addWidget(m_udpIn, 0, 2);
    tiles->addWidget(m_udpLost, 0, 3);
    tiles->addWidget(m_errors, 0, 4);
    m_recent = new QPlainTextEdit(box);
    m_recent->setObjectName(QStringLiteral("psJournalRecent"));
    m_recent->setReadOnly(true);
    m_recent->setMaximumBlockCount(kRecentLines);
    m_recent->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_recent->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_recent->setFixedHeight(130);
    auto* open = new QPushButton(QStringLiteral("Открыть журнал…"), box);
    open->setObjectName(QStringLiteral("psOpenJournal"));
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(open);
    layout->addLayout(tiles);
    layout->addWidget(m_recent);
    layout->addLayout(row);
    connect(open, &QPushButton::clicked, this, &ConnectionTab::openJournal);
    return box;
}

QWidget* ConnectionTab::buildCommandBox()
{
    m_commandTabs = new QTabWidget(this);
    m_commandTabs->setObjectName(QStringLiteral("connectionCommandTabs"));
    m_packetPage = new PsPacketPage(m_session, m_commandTabs);
    m_commandTabs->addTab(m_packetPage, QStringLiteral("Одна команда"));
    auto* journalPage = new QWidget(m_commandTabs);
    auto* journalLayout = new QVBoxLayout(journalPage);
    auto* note = new QLabel(QStringLiteral("Журнал обмена открывается отдельным окном."), journalPage);
    auto* open = new QPushButton(QStringLiteral("Открыть журнал…"), journalPage);
    journalLayout->addWidget(note);
    journalLayout->addWidget(open);
    journalLayout->addStretch(1);
    m_commandTabs->addTab(journalPage, QStringLiteral("Журнал"));
    connect(open, &QPushButton::clicked, this, &ConnectionTab::openJournal);
    // Нажали «Журнал» — открываем модальное окно и возвращаемся к «Одной команде».
    connect(m_commandTabs, &QTabWidget::currentChanged, this, [this, journalPage](int index) {
        if (m_commandTabs->widget(index) == journalPage) {
            m_commandTabs->setCurrentWidget(m_packetPage);
            openJournal();
        }
    });
    connect(m_session, &PsSession::controlsChanged, this,
            [this] { m_packetPage->setAllowed(m_session->manualAllowed()); });
    m_packetPage->setAllowed(m_session->manualAllowed());
    return m_commandTabs;
}

void ConnectionTab::addTechPage(QWidget* page, const QString& title)
{
    m_techTabs->addTab(page, title);
}

QString ConnectionTab::macText() const
{
    return MacResolver::formatMac(m_mac->text().trimmed());
}

void ConnectionTab::openJournal()
{
    m_journal->open();
    m_journal->raise();
}

void ConnectionTab::onConnectClicked()
{
    PsLink* link = m_session->link();
    if (link->state() != PsLink::State::Disconnected || m_session->searching()) {
        m_session->disconnectFromPs();
        return;
    }
    m_session->connectTo(m_host->text(), static_cast<quint16>(m_port->value()),
                         static_cast<quint16>(m_tlmPort->value()), m_mac->text());
}

void ConnectionTab::refreshConnection()
{
    const PsLink::State state = m_session->link()->state();
    const bool idle = state == PsLink::State::Disconnected && !m_session->searching();
    const bool ready = state == PsLink::State::Ready;
    m_linkState->setText(m_session->statusText());
    m_connect->setText(idle ? QStringLiteral("Подключиться")
                            : (ready ? QStringLiteral("Отключиться") : QStringLiteral("Отмена")));
    m_connect->setEnabled(!m_session->seriesActive());
    m_findByMac->setEnabled(idle && !m_session->seriesActive());
    for (QWidget* w : {static_cast<QWidget*>(m_host), static_cast<QWidget*>(m_mac),
                       static_cast<QWidget*>(m_port), static_cast<QWidget*>(m_tlmPort)}) {
        w->setEnabled(idle);
    }
    emit summaryChanged(ready ? QStringLiteral("PS: OK") : QStringLiteral("PS: нет связи"), ready);
    refreshTech();
}

void ConnectionTab::refreshTech()
{
    while (QLayoutItem* item = m_techTiles->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!m_session->link()->isReady()) {
        m_techMode->setText(QStringLiteral("нет данных: PS не подключён"));
        m_techValues->clear();
        return;
    }
    QString mode = m_session->modeKnown()
                       ? QString::fromStdString(ps::modeRu(m_session->mode()))
                       : QStringLiteral("режим неизвестен");
    if (!m_session->state().isEmpty()) {
        mode += QStringLiteral(", %1").arg(QString::fromStdString(ps::stateRu(m_session->state().toStdString())));
    }
    m_techMode->setText(QStringLiteral("Режим: %1").arg(mode));
    const auto& t = m_session->telemetry();
    if (!t) {
        m_techValues->setText(QStringLiteral("телеметрия UDP %1 ещё не приходила")
                                  .arg(m_session->link()->telemetryPort()));
        return;
    }
    int i = 0;
    for (const auto& [id, health] : t->devices) {
        const char* tone = health == ps::DeviceHealth::Ok     ? "ok"
                           : health == ps::DeviceHealth::Fail ? "fail"
                                                              : "none";
        auto* label = tile(this, QString::fromStdString(ps::selfTestRu(id)), tone);
        const std::string_view healthText = ps::toString(health);
        label->setToolTip(QStringLiteral("%1: %2").arg(
            QString::fromStdString(id),
            QString::fromUtf8(healthText.data(), static_cast<qsizetype>(healthText.size()))));
        m_techTiles->addWidget(label, i / 4, i % 4);
        ++i;
    }
    QStringList lines;
    QStringList temps;
    for (const auto& [id, v] : t->temperature) {
        temps << QStringLiteral("%1 %2 °C").arg(QString::fromStdString(id)).arg(v, 0, 'f', 1);
    }
    if (!temps.isEmpty()) {
        lines << QStringLiteral("Температура: %1").arg(temps.join(QStringLiteral(" · ")));
    }
    QStringList power;
    for (const auto& [id, v] : t->power) {
        power << QStringLiteral("%1 %2 В").arg(QString::fromStdString(id)).arg(v, 0, 'f', 2);
    }
    if (!power.isEmpty()) {
        lines << QStringLiteral("Питание: %1").arg(power.join(QStringLiteral(" · ")));
    }
    lines << QStringLiteral("Работа: %1").arg(uptimeText(t->uptime));
    m_techValues->setText(lines.join(QLatin1Char('\n')));
}

void ConnectionTab::refreshCounters()
{
    const PsLink* link = m_session->link();
    m_tcpOut->setText(QStringLiteral("TCP ушло\n%1").arg(m_tcpOutCount));
    m_tcpIn->setText(QStringLiteral("TCP пришло\n%1").arg(m_tcpInCount));
    m_udpIn->setText(QStringLiteral("UDP принято\n%1").arg(link->telemetryPackets()));
    m_udpLost->setText(QStringLiteral("UDP потеряно\n%1").arg(link->telemetryLost()));
    m_errors->setText(QStringLiteral("Сообщений\n%1").arg(m_errorCount));
}
