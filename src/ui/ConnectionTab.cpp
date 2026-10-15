#include "ConnectionTab.h"

#include "MacResolver.h"
#include "PsExchangeView.h"
#include "PsJournalDialog.h"
#include "PsPacketPage.h"
#include "PsSession.h"
#include "UiStyle.h"

#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QSpinBox>
#include <QVBoxLayout>

namespace ps = afar::ps;

namespace {


}  // namespace

ConnectionTab::ConnectionTab(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("connectionTab"));
    auto* root = new QGridLayout(this);
    root->setContentsMargins(ui::kPageMargin, ui::kPageMargin, ui::kPageMargin, ui::kPageMargin);
    root->setHorizontalSpacing(ui::kGap);
    root->setVerticalSpacing(ui::kGap);

    // Слева: подключение и тех. состояние рядом, под ними — «Одна команда».
    root->addWidget(buildConnectionBox(), 0, 0);
    root->addWidget(buildTechBox(), 0, 1);
    root->addWidget(buildCommandBox(), 1, 0, 1, 2);
    // Справа журнал обмена — во всю высоту.
    root->addWidget(buildJournalBox(), 0, 2, 2, 1);

    root->setColumnStretch(0, 3);
    root->setColumnStretch(1, 4);
    root->setColumnStretch(2, 6);
    root->setRowStretch(0, 0);
    root->setRowStretch(1, 1);

    m_journal = new PsJournalDialog(m_session, this);

    connect(m_session, &PsSession::statusChanged, this, &ConnectionTab::refreshConnection);
    connect(m_session, &PsSession::controlsChanged, this, &ConnectionTab::refreshConnection);
    connect(m_session, &PsSession::hostResolved, this, [this](const QString& ip) {
        // Найденный адрес подставляем, но не затираем то, что оператор сейчас печатает.
        if (!m_host->hasFocus()) {
            m_host->setText(ip);
        }
    });
    connect(m_session, &PsSession::boardMacDetected, this, [this](const QString& mac) {
        if (!m_mac->hasFocus()) {
            m_mac->setText(mac);
        }
        m_message->setText(QStringLiteral("MAC платы определён по таблице ARP и подставлен: %1").arg(mac));
    });
    connect(m_session, &PsSession::boardCandidate, this, [this](const QString& ip, const QString& mac) {
        if (!m_host->hasFocus()) {
            m_host->setText(ip);
        }
        if (!mac.isEmpty() && !m_mac->hasFocus()) {
            m_mac->setText(mac);
        }
        m_message->setText(m_message->text() + QStringLiteral(" Адрес %1%2 подставлен — нажмите «Подключиться».")
                                                   .arg(ip, mac.isEmpty() ? QString() : QStringLiteral(" и MAC %1").arg(mac)));
    });
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
        refreshCounters();
    });
    connect(m_session, &PsSession::logCleared, this, [this] {
        m_tcpOutCount = 0;
        m_tcpInCount = 0;
        m_errorCount = 0;
        refreshCounters();
    });

    refreshConnection();
    refreshCounters();
}

QWidget* ConnectionTab::buildConnectionBox()
{
    auto* box = new QGroupBox(QStringLiteral("Подключение к плате"), this);
    box->setObjectName(QStringLiteral("connectionBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setSpacing(ui::kInnerGap);

    m_host = new QLineEdit(m_session->host(), box);
    m_host->setObjectName(QStringLiteral("psHost"));
    m_host->setPlaceholderText(QStringLiteral("например 172.16.226.99"));
    m_host->setClearButtonEnabled(true);
    m_host->setToolTip(QStringLiteral("IP-адрес платы. Если он пустой или плата по нему не отвечает, "
                                      "плата ищется по MAC-адресу в локальной сети."));
    m_mac = new QLineEdit(m_session->mac(), box);
    m_mac->setObjectName(QStringLiteral("psMac"));
    m_mac->setPlaceholderText(QStringLiteral("AA:BB:CC:DD:EE:FF"));
    m_mac->setToolTip(QStringLiteral("IP платы меняется, MAC постоянный: по нему плату находят в сети."));

    m_port = new QSpinBox(box);
    m_port->setObjectName(QStringLiteral("psPort"));
    m_port->setRange(1, 65535);
    m_port->setValue(m_session->port());
    m_port->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_port->setToolTip(QStringLiteral("TCP-порт канала управления PS"));
    m_tlmPort = new QSpinBox(box);
    m_tlmPort->setObjectName(QStringLiteral("psTlmPort"));
    m_tlmPort->setRange(1, 65535);
    m_tlmPort->setValue(m_session->telemetryPort());
    m_tlmPort->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_tlmPort->setToolTip(QStringLiteral("UDP-порт, на который PS присылает телеметрию"));

    auto* ports = new QGridLayout;
    ports->setHorizontalSpacing(ui::kInnerGap);
    ports->setVerticalSpacing(2);
    ports->addWidget(ui::captionLabel(QStringLiteral("TCP-порт"), box), 0, 0);
    ports->addWidget(ui::captionLabel(QStringLiteral("UDP-порт"), box), 0, 1);
    ports->addWidget(m_port, 1, 0);
    ports->addWidget(m_tlmPort, 1, 1);

    m_linkState = new QLabel(box);
    m_linkState->setObjectName(QStringLiteral("psLinkState"));
    m_linkState->setProperty("role", QStringLiteral("status"));
    m_linkState->setWordWrap(true);

    m_connect = new QPushButton(QStringLiteral("Подключиться"), box);
    m_connect->setObjectName(QStringLiteral("psConnect"));
    ui::setButtonRole(m_connect, ui::ButtonRole::Primary);
    m_findByMac = new QPushButton(QStringLiteral("Найти по MAC"), box);
    m_findByMac->setObjectName(QStringLiteral("psFindByMac"));
    ui::setButtonRole(m_findByMac, ui::ButtonRole::Secondary);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(ui::kInnerGap);
    buttons->addWidget(m_connect, 1);
    buttons->addWidget(m_findByMac, 1);

    m_message = ui::hintLabel(QString(), box);
    m_message->setObjectName(QStringLiteral("psMessage"));

    layout->addWidget(ui::captionLabel(QStringLiteral("IP-адрес"), box));
    layout->addWidget(m_host);
    layout->addWidget(ui::captionLabel(QStringLiteral("MAC-адрес"), box));
    layout->addWidget(m_mac);
    layout->addLayout(ports);
    layout->addWidget(m_linkState);
    layout->addLayout(buttons);
    layout->addWidget(m_message);
    layout->addStretch(1);

    connect(m_connect, &QPushButton::clicked, this, &ConnectionTab::onMainButton);
    // Enter в любом поле — подключиться с тем, что введено (и повторить попытку, если она уже идёт).
    for (QLineEdit* edit : {m_host, m_mac}) {
        connect(edit, &QLineEdit::returnPressed, this, &ConnectionTab::connectNow);
    }
    for (QSpinBox* spin : {m_port, m_tlmPort}) {
        connect(spin, &QSpinBox::editingFinished, this, [this, spin] {
            if (spin->hasFocus()) {
                connectNow();
            }
        });
    }
    connect(m_findByMac, &QPushButton::clicked, this, [this] {
        m_session->findByMac(m_mac->text().trimmed(), static_cast<quint16>(m_port->value()),
                             static_cast<quint16>(m_tlmPort->value()));
    });
    return box;
}

QWidget* ConnectionTab::buildTechBox()
{
    auto* box = new QGroupBox(QStringLiteral("Тех. состояние"), this);
    box->setObjectName(QStringLiteral("techBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    m_techBody = new QWidget(box);
    auto* bodyLayout = new QVBoxLayout(m_techBody);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_techBody, 1);
    return box;
}

QWidget* ConnectionTab::buildJournalBox()
{
    auto* box = new QGroupBox(QStringLiteral("Журнал обмена"), this);
    box->setObjectName(QStringLiteral("journalBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setSpacing(ui::kInnerGap);

    auto* tiles = new QHBoxLayout;
    tiles->setSpacing(ui::kInnerGap);
    m_tcpOut = new StatTile(QStringLiteral("TCP ушло"), box);
    m_tcpIn = new StatTile(QStringLiteral("TCP пришло"), box);
    m_udpIn = new StatTile(QStringLiteral("UDP принято"), box);
    m_udpLost = new StatTile(QStringLiteral("UDP потеряно"), box);
    m_errors = new StatTile(QStringLiteral("Сообщений"), box);
    m_tcpOut->setObjectName(QStringLiteral("psTileTcpOut"));
    m_tcpIn->setObjectName(QStringLiteral("psTileTcpIn"));
    m_udpIn->setObjectName(QStringLiteral("psTileUdpIn"));
    m_udpLost->setObjectName(QStringLiteral("psTileUdpLost"));
    m_errors->setObjectName(QStringLiteral("psJournalErrors"));
    for (StatTile* tile : {m_tcpOut, m_tcpIn, m_udpIn, m_udpLost, m_errors}) {
        tiles->addWidget(tile, 1);
    }

    // Последние пакеты — таблицей; выбранный пакет виден целиком под ней.
    m_exchange = new PsExchangeView(m_session, box, true);
    m_exchange->table()->setObjectName(QStringLiteral("psJournalRecent"));
    auto* open = new QPushButton(QStringLiteral("Открыть журнал…"), box);
    open->setObjectName(QStringLiteral("psOpenJournal"));
    ui::setButtonRole(open, ui::ButtonRole::Secondary);
    auto* row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(open);

    layout->addLayout(tiles);
    layout->addWidget(m_exchange, 1);
    layout->addLayout(row);
    connect(open, &QPushButton::clicked, this, &ConnectionTab::openJournal);
    return box;
}

QWidget* ConnectionTab::buildCommandBox()
{
    auto* box = new QGroupBox(QStringLiteral("Обмен сообщениями с PS"), this);
    box->setObjectName(QStringLiteral("commandBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kInnerGap);

    // keep_alive: как часто проверяем связь, пока по каналу ничего не идёт.
    auto* keepRow = new QHBoxLayout;
    keepRow->setSpacing(ui::kInnerGap);
    m_keepAlive = new QSpinBox(box);
    m_keepAlive->setObjectName(QStringLiteral("psKeepAlive"));
    m_keepAlive->setRange(PsSession::kKeepAliveMinMs, PsSession::kKeepAliveMaxMs);
    m_keepAlive->setSingleStep(100);
    m_keepAlive->setSuffix(QStringLiteral(" мс"));
    m_keepAlive->setValue(m_session->keepAliveMs());
    m_keepAlive->setFixedWidth(110);
    m_keepAlive->setToolTip(QStringLiteral("Если по каналу TCP ничего не идёт, PS проверяется командой keep_alive "
                                           "с этим интервалом. Не ответил — связь считается потерянной."));
    m_keepAliveHint = ui::hintLabel(QString(), box);
    m_keepAliveHint->setObjectName(QStringLiteral("psKeepAliveHint"));
    keepRow->addWidget(new QLabel(QStringLiteral("keep_alive при простое канала каждые"), box));
    keepRow->addWidget(m_keepAlive);
    keepRow->addWidget(m_keepAliveHint, 1);
    layout->addLayout(keepRow);
    const auto refreshKeepHint = [this] {
        const double perSecond = 1000.0 / m_keepAlive->value();
        m_keepAliveHint->setText(QStringLiteral("≈ %1 раз в секунду, нет ответа — разрыв через %2 с")
                                     .arg(perSecond, 0, 'f', perSecond < 1.0 ? 2 : 1)
                                     .arg((m_keepAlive->value() + 3000) / 1000.0, 0, 'f', 1));
    };
    refreshKeepHint();
    connect(m_keepAlive, &QSpinBox::valueChanged, this, [this, refreshKeepHint](int ms) {
        m_session->setKeepAliveMs(ms);
        refreshKeepHint();
    });

    m_packetPage = new PsPacketPage(m_session, box);
    layout->addWidget(m_packetPage);
    connect(m_session, &PsSession::controlsChanged, this,
            [this] { m_packetPage->setAllowed(m_session->manualAllowed()); });
    m_packetPage->setAllowed(m_session->manualAllowed());
    return box;
}

void ConnectionTab::setTechPage(QWidget* page)
{
    page->setParent(m_techBody);
    m_techBody->layout()->addWidget(page);
}

void ConnectionTab::addTechSection(QWidget* section)
{
    section->setParent(m_techBody);
    m_techBody->layout()->addWidget(section);
}

void ConnectionTab::openJournal()
{
    m_journal->open();
    m_journal->raise();
}

QString ConnectionTab::macText() const
{
    return MacResolver::formatMac(m_mac->text().trimmed());
}

void ConnectionTab::connectNow()
{
    if (m_session->link()->isReady() || m_session->seriesActive()) {
        return;
    }
    m_session->connectTo(m_host->text(), static_cast<quint16>(m_port->value()),
                         static_cast<quint16>(m_tlmPort->value()), m_mac->text());
}

void ConnectionTab::onMainButton()
{
    // Связь есть — отключиться; идёт попытка или поиск — отменить; иначе подключиться.
    if (m_session->link()->isReady() || m_session->busy()) {
        m_session->disconnectFromPs();
        return;
    }
    connectNow();
}

void ConnectionTab::refreshConnection()
{
    const bool ready = m_session->link()->isReady();
    const bool busy = m_session->busy();
    const bool series = m_session->seriesActive();
    m_linkState->setText(m_session->statusText());
    ui::setTone(m_linkState, ready ? ui::Tone::Ok
                             : busy ? ui::Tone::Info
                             : m_session->lastError().isEmpty() && m_session->link()->lastError().isEmpty()
                                 ? ui::Tone::Neutral
                                 : ui::Tone::Error);
    m_connect->setText(ready ? QStringLiteral("Отключиться")
                             : busy ? QStringLiteral("Отмена") : QStringLiteral("Подключиться"));
    ui::setButtonRole(m_connect, ready || busy ? ui::ButtonRole::Danger : ui::ButtonRole::Primary);
    m_connect->setEnabled(!series);
    // Пока связи нет, поля можно править всегда — и во время попыток подключения, и во время поиска по MAC.
    m_findByMac->setEnabled(!ready && !series);
    for (QWidget* w : {static_cast<QWidget*>(m_host), static_cast<QWidget*>(m_mac),
                       static_cast<QWidget*>(m_port), static_cast<QWidget*>(m_tlmPort)}) {
        w->setEnabled(!ready);
    }
}

void ConnectionTab::refreshCounters()
{
    const PsLink* link = m_session->link();
    m_tcpOut->setValue(QString::number(m_tcpOutCount));
    m_tcpIn->setValue(QString::number(m_tcpInCount));
    m_udpIn->setValue(QString::number(link->telemetryPackets()));
    m_udpLost->setValue(QString::number(link->telemetryLost()));
    m_udpLost->setTone(link->telemetryLost() > 0 ? ui::Tone::Warn : ui::Tone::Neutral);
    m_errors->setValue(QString::number(m_errorCount));
    m_errors->setTone(m_errorCount > 0 ? ui::Tone::Warn : ui::Tone::Neutral);
}
