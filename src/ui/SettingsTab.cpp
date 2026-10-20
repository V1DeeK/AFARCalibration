#include "SettingsTab.h"

#include "BoardParametersPage.h"
#include "FullScanPanel.h"
#include "MacResolver.h"
#include "OperationalPanel.h"
#include "PsDiagnosticsPage.h"
#include "PsExchangeView.h"
#include "PsJournalDialog.h"
#include "PsModulesPage.h"
#include "PsOsdPage.h"
#include "PsPacketPage.h"
#include "PsSession.h"
#include "UiStyle.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

constexpr int kLogLines = 500;

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

}  // namespace

SettingsTab::SettingsTab(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("settingsTab"));
    auto* modulesBox = new QGroupBox(QStringLiteral("Модули"), this);
    modulesBox->setObjectName(QStringLiteral("modulesBox"));
    auto* modulesLayout = new QVBoxLayout(modulesBox);
    modulesLayout->setContentsMargins(0, 0, 0, 0);
    m_modulesPage = new PsModulesPage(m_session->link(), modulesBox);
    m_modulesPage->tabs()->addTab(buildCodesPage(), QStringLiteral("Коды решётки"));
    m_osd = new PsOsdPage(m_session, m_modulesPage->tabs());
    m_modulesPage->tabs()->addTab(ui::scrollPage(m_osd, m_modulesPage->tabs()), QStringLiteral("OSD"));
    modulesLayout->addWidget(m_modulesPage);

    auto* scanBox = new QGroupBox(QStringLiteral("Полное сканирование"), this);
    scanBox->setObjectName(QStringLiteral("scanBox"));
    auto* scanLayout = new QVBoxLayout(scanBox);
    scanLayout->setContentsMargins(0, 0, 0, 0);
    m_fullScan = new FullScanPanel(m_session, scanBox);
    scanLayout->addWidget(m_fullScan);

    auto* opBox = new QGroupBox(QStringLiteral("Оперативный режим"), this);
    opBox->setObjectName(QStringLiteral("opBox"));
    opBox->setToolTip(QStringLiteral("Команды v8 п. 3.5 — в режиме OPERATIONAL, не только в сервисном"));
    auto* opLayout = new QVBoxLayout(opBox);
    opLayout->setContentsMargins(0, 0, 0, 0);
    m_operational = new OperationalPanel(m_session, opBox);
    opLayout->addWidget(m_operational);

    // Сверху в ряд: подключение, тех. состояние (узкое), оперативный режим и режим изделия.
    // Ниже слева «Модули», под ними «Полное сканирование» и рядом «Луч»; справа журнал.
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ui::kPageMargin, ui::kPageMargin, ui::kPageMargin, ui::kPageMargin);
    root->setSpacing(ui::kGap);

    auto* top = new QHBoxLayout;
    top->setSpacing(ui::kGap);
    top->addWidget(buildConnectionBox(), 8);
    top->addWidget(buildTechBox(), 7);
    top->addWidget(opBox, 18);
    top->addWidget(buildModeBox(), 6);
    root->addLayout(top);

    auto* left = new QVBoxLayout;
    left->setSpacing(ui::kGap);
    left->addWidget(modulesBox, 3);
    auto* scanRow = new QHBoxLayout;
    scanRow->setSpacing(ui::kGap);
    scanRow->addWidget(scanBox, 3);
    scanRow->addWidget(buildBeamBox(), 1);
    left->addLayout(scanRow, 2);

    auto* bottom = new QHBoxLayout;
    bottom->setSpacing(ui::kGap);
    bottom->addLayout(left, 3);
    bottom->addWidget(buildJournalBox(), 2);
    root->addLayout(bottom, 1);

    m_journal = new PsJournalDialog(m_session, this);

    connect(m_session, &PsSession::statusChanged, this, &SettingsTab::refreshConnection);
    connect(m_session, &PsSession::controlsChanged, this, &SettingsTab::refreshConnection);
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
        ++m_messageCount;
        refreshCounters();
        log(text);
    });
    connect(m_session, &PsSession::frameAppended, this, [this](const PsSession::LogLine& l) {
        if (l.channel == QLatin1String("TCP")) {
            ++(l.outgoing ? m_tcpOutCount : m_tcpInCount);
        }
        refreshCounters();
    });
    connect(m_session, &PsSession::logCleared, this, [this] {
        m_tcpOutCount = 0;
        m_tcpInCount = 0;
        m_messageCount = 0;
        m_log->clear();
        refreshCounters();
    });

    connect(m_session->link(), &PsLink::responseReceived, this, &SettingsTab::onResponse);
    connect(m_session, &PsSession::telemetryChanged, this, &SettingsTab::onTelemetry);
    connect(m_session, &PsSession::modeChanged, this, &SettingsTab::refreshMode);
    connect(m_session, &PsSession::controlsChanged, this, &SettingsTab::refreshEnabled);
    refreshConnection();
    refreshCounters();
    refreshMode();
}

SettingsTab::~SettingsTab() = default;

QWidget* SettingsTab::buildJournalBox()
{
    auto* box = new QGroupBox(QStringLiteral("Журнал"), this);
    box->setObjectName(QStringLiteral("journalBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setSpacing(ui::kInnerGap);

    auto* tiles = new QHBoxLayout;
    tiles->setSpacing(ui::kInnerGap);
    m_tcpOut = new StatTile(QStringLiteral("TCP отправлено"), box);
    m_tcpIn = new StatTile(QStringLiteral("TCP принято"), box);
    m_udpIn = new StatTile(QStringLiteral("UDP принято"), box);
    m_udpLost = new StatTile(QStringLiteral("UDP отброшено"), box);
    m_messages = new StatTile(QStringLiteral("Сообщения"), box);
    m_tcpOut->setObjectName(QStringLiteral("psTileTcpOut"));
    m_tcpIn->setObjectName(QStringLiteral("psTileTcpIn"));
    m_udpIn->setObjectName(QStringLiteral("psTileUdpIn"));
    m_udpLost->setObjectName(QStringLiteral("psTileUdpLost"));
    m_messages->setObjectName(QStringLiteral("psJournalErrors"));
    m_messages->setToolTip(QStringLiteral("Ошибки связи, отказы PS и сообщения программы; "
                                          "сами сообщения — под таблицей обмена"));
    for (StatTile* tile : {m_tcpOut, m_tcpIn, m_udpIn, m_udpLost, m_messages}) {
        tiles->addWidget(tile, 1);
    }

    m_journalTabs = new QTabWidget(box);
    m_journalTabs->setObjectName(QStringLiteral("journalTabs"));
    m_journalTabs->setUsesScrollButtons(false);
    m_journalTabs->setMinimumHeight(360);

    auto* exchangePage = new QWidget(m_journalTabs);
    auto* exchangeLayout = new QVBoxLayout(exchangePage);
    exchangeLayout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    exchangeLayout->setSpacing(ui::kInnerGap);
    m_exchange = new PsExchangeView(m_session, exchangePage);
    m_exchange->table()->setObjectName(QStringLiteral("psExchange"));
    m_log = new QPlainTextEdit(exchangePage);
    m_log->setObjectName(QStringLiteral("psLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setFixedHeight(64);
    m_log->setPlaceholderText(QStringLiteral("ошибки связи, отказы PS и сообщения программы"));
    auto* open = new QPushButton(QStringLiteral("Открыть журнал…"), exchangePage);
    open->setObjectName(QStringLiteral("psOpenJournal"));
    open->setToolTip(QStringLiteral("Весь обмен в отдельном окне и команда одной строкой"));
    ui::setButtonRole(open, ui::ButtonRole::Secondary);
    auto* logRow = new QHBoxLayout;
    logRow->setSpacing(ui::kInnerGap);
    logRow->addWidget(m_log, 1);
    logRow->addWidget(open, 0, Qt::AlignBottom);
    // Как на эскизе: под заголовками вкладок — счётчики, под ними таблица обмена.
    exchangeLayout->addLayout(tiles);
    exchangeLayout->addWidget(m_exchange, 1);
    exchangeLayout->addLayout(logRow);
    m_journalTabs->addTab(exchangePage, QStringLiteral("Журнал обмена"));
    connect(open, &QPushButton::clicked, this, &SettingsTab::openJournal);

    auto* tlmPage = new QWidget(m_journalTabs);
    auto* tlmLayout = new QVBoxLayout(tlmPage);
    tlmLayout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    tlmLayout->setSpacing(ui::kInnerGap);
    m_tlmCounters = new QLabel(QStringLiteral("датаграмм нет"), tlmPage);
    m_tlmCounters->setObjectName(QStringLiteral("psTlmCounters"));
    m_tlmCounters->setProperty("role", QStringLiteral("hint"));
    m_tlmTable = new QTableWidget(0, 2, tlmPage);
    m_tlmTable->setObjectName(QStringLiteral("psTlmTable"));
    m_tlmTable->setHorizontalHeaderLabels({QStringLiteral("Параметр"), QStringLiteral("Значение")});
    m_tlmTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tlmTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tlmTable->verticalHeader()->setVisible(false);
    m_tlmTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tlmTable->setAlternatingRowColors(true);
    m_tlmTable->setShowGrid(false);
    tlmLayout->addWidget(m_tlmCounters);
    tlmLayout->addWidget(m_tlmTable, 1);
    m_journalTabs->addTab(tlmPage, QStringLiteral("Телеметрия"));

    m_journalTabs->addTab(buildSingleMessagePage(), QStringLiteral("Одиночное сообщение"));
    layout->addWidget(m_journalTabs, 1);
    return box;
}

QWidget* SettingsTab::buildSingleMessagePage()
{
    auto* page = new QWidget(this);
    page->setObjectName(QStringLiteral("commandBox"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, ui::kGap, 0, 0);
    layout->setSpacing(ui::kInnerGap);

    // keep_alive: как часто проверяем связь, пока по каналу ничего не идёт.
    auto* keepRow = new QHBoxLayout;
    keepRow->setContentsMargins(ui::kGap, 0, ui::kGap, 0);
    keepRow->setSpacing(ui::kInnerGap);
    m_keepAlive = new QSpinBox(page);
    m_keepAlive->setObjectName(QStringLiteral("psKeepAlive"));
    m_keepAlive->setRange(PsSession::kKeepAliveMinMs, PsSession::kKeepAliveMaxMs);
    m_keepAlive->setSingleStep(100);
    m_keepAlive->setSuffix(QStringLiteral(" мс"));
    m_keepAlive->setValue(m_session->keepAliveMs());
    m_keepAlive->setFixedWidth(110);
    m_keepAlive->setToolTip(QStringLiteral("Если по каналу TCP ничего не идёт, PS проверяется командой keep_alive "
                                           "с этим интервалом. Не ответил — связь считается потерянной."));
    m_keepAliveHint = ui::hintLabel(QString(), page);
    m_keepAliveHint->setObjectName(QStringLiteral("psKeepAliveHint"));
    keepRow->addWidget(new QLabel(QStringLiteral("keep_alive при простое канала каждые"), page));
    keepRow->addWidget(m_keepAlive);
    keepRow->addWidget(m_keepAliveHint, 1);
    auto* keepNow = new QPushButton(QStringLiteral("Отправить keep_alive"), page);
    keepNow->setObjectName(QStringLiteral("psKeepAliveNow"));
    keepNow->setToolTip(QStringLiteral("keep_alive сейчас, не дожидаясь простоя канала (v8 п. 3.2.1.2)"));
    ui::setButtonRole(keepNow, ui::ButtonRole::Secondary);
    keepRow->addWidget(keepNow);
    layout->addLayout(keepRow);
    connect(keepNow, &QPushButton::clicked, this, [this] {
        if (m_session->send(ps::msg::KeepAlive) >= 0) {
            log(QStringLiteral("keep_alive отправлена"));
        }
    });
    connect(m_session, &PsSession::controlsChanged, keepNow,
            [this, keepNow] { keepNow->setEnabled(m_session->manualAllowed()); });
    keepNow->setEnabled(m_session->manualAllowed());
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

    m_packetPage = new PsPacketPage(m_session, page);
    layout->addWidget(m_packetPage, 1);
    connect(m_session, &PsSession::controlsChanged, this,
            [this] { m_packetPage->setAllowed(m_session->manualAllowed()); });
    m_packetPage->setAllowed(m_session->manualAllowed());
    return page;
}

QTableWidget* SettingsTab::exchangeTable() const
{
    return m_exchange->table();
}

QWidget* SettingsTab::buildModeBox()
{
    auto* box = new QGroupBox(QStringLiteral("Режим изделия"), this);
    box->setObjectName(QStringLiteral("modeBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setSpacing(ui::kInnerGap);
    m_modeLabel = new QLabel(QStringLiteral("режим: —"), box);
    m_modeLabel->setObjectName(QStringLiteral("psMode"));
    m_modeLabel->setProperty("role", QStringLiteral("status"));
    m_modeLabel->setWordWrap(true);
    // Подсказки — по v8 п. 1 (таблица 1) и п. 1.1–1.3: откуда в режим можно войти и как выйти.
    m_toService = new QPushButton(QStringLiteral("Перейти в сервисный"), box);
    m_toService->setObjectName(QStringLiteral("psToService"));
    m_toService->setToolTip(QStringLiteral(
        "<b>SERVICE — измерительный режим.</b><br>Настройка модулей, измерения и калибровка на стенде, "
        "запись параметров при испытаниях.<br><b>Вход:</b> только из ожидания (IDLE, состояние READY).<br>"
        "<b>Выход:</b> в ожидание (IDLE); PS сам восстанавливает рабочую конфигурацию."));
    m_toOperational = new QPushButton(QStringLiteral("Перейти в оперативный"), box);
    m_toOperational->setObjectName(QStringLiteral("psToOperational"));
    m_toOperational->setToolTip(QStringLiteral(
        "<b>OPERATIONAL — оперативный режим.</b><br>Поиск, захват и сопровождение источника: выдача "
        "пеленга и класса источника в реальном времени.<br><b>Вход:</b> из ожидания (IDLE) или из "
        "полного сканирования (FULL_SCAN_FREQ).<br><b>Выход:</b> в ожидание или в полное сканирование, "
        "только из состояний «установка частоты» (OP_INITIALIZATION) и «ожидание команды» (OP_IDLE); "
        "идущий поиск или сопровождение сначала останавливают кнопкой «Стоп»."));
    m_toIdle = new QPushButton(QStringLiteral("Вернуть в ожидание"), box);
    m_toIdle->setObjectName(QStringLiteral("psToIdle"));
    m_toIdle->setToolTip(QStringLiteral(
        "<b>IDLE — режим ожидания.</b><br>Безопасная точка: через него проходят все смены режима, "
        "аппаратура в безопасной конфигурации; здесь доступны самопроверка и перезапуск.<br>"
        "<b>Вход:</b> из любого режима (в оперативном — из OP_INITIALIZATION или OP_IDLE, в полном "
        "сканировании — когда оно остановлено).<br><b>Выход:</b> в сервисный, оперативный или "
        "полное сканирование."));
    m_getState = new QPushButton(QStringLiteral("Запросить состояние"), box);
    m_getState->setObjectName(QStringLiteral("psGetState"));
    m_getState->setToolTip(QStringLiteral("get_state: текущий режим и состояние вложенного автомата"));
    ui::setButtonRole(m_toService, ui::ButtonRole::Primary);
    ui::setButtonRole(m_toOperational, ui::ButtonRole::Primary);
    ui::setButtonRole(m_toIdle, ui::ButtonRole::Secondary);
    ui::setButtonRole(m_getState, ui::ButtonRole::Secondary);
    m_selfTest = new QPushButton(QStringLiteral("Запустить самопроверку"), box);
    m_selfTest->setObjectName(QStringLiteral("psStartTest"));
    m_selfTest->setToolTip(QStringLiteral("set_start_test: ответ придёт после самопроверки, "
                                          "результаты — в техническом состоянии (биты 72–79)"));
    m_reset = new QPushButton(QStringLiteral("Перезапустить изделие…"), box);
    m_reset->setObjectName(QStringLiteral("psReset"));
    m_reset->setToolTip(QStringLiteral("set_reset: изделие ответит, закроет TCP и перезапустится"));
    ui::setButtonRole(m_selfTest, ui::ButtonRole::Secondary);
    ui::setButtonRole(m_reset, ui::ButtonRole::Danger);
    layout->addWidget(m_modeLabel);
    layout->addWidget(m_toService);
    layout->addWidget(m_toOperational);
    layout->addWidget(m_toIdle);
    layout->addWidget(m_getState);
    // Кнопки друг под другом: окошко узкое, рядом с «Тех. состоянием» и «Лучом».
    layout->addWidget(m_selfTest);
    layout->addWidget(m_reset);
    layout->addWidget(ui::hintLabel(
        QStringLiteral("Наведите курсор на кнопку режима: там сказано, для чего режим, "
                       "откуда в него можно войти и как выйти."),
        box));
    layout->addStretch(1);

    connect(m_toService, &QPushButton::clicked, this,
            [this] { m_session->requestMode(ps::Mode::Service); });
    connect(m_toOperational, &QPushButton::clicked, this,
            [this] { m_session->requestMode(ps::Mode::Operational); });
    connect(m_toIdle, &QPushButton::clicked, this,
            [this] { m_session->requestMode(ps::Mode::Idle); });
    connect(m_getState, &QPushButton::clicked, this, [this] { send(ps::msg::GetState); });
    connect(m_selfTest, &QPushButton::clicked, this, &SettingsTab::startSelfTest);
    connect(m_reset, &QPushButton::clicked, this, [this] {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Перезапуск изделия"),
            QStringLiteral("Изделие перезапустится: связь с PS прервётся, после перезапуска "
                           "пройдут инициализация и самопроверка. Перезапустить?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer == QMessageBox::Yes) {
            resetBoard();
        }
    });
    return box;
}

QWidget* SettingsTab::buildCodesPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kInnerGap);
    m_codes = new QTableWidget(ps::kChannels, 7, page);
    m_codes->setObjectName(QStringLiteral("psCodes"));
    m_codes->setMinimumHeight(200);
    m_codes->setHorizontalHeaderLabels({QStringLiteral("Канал"), QStringLiteral("Фаза: задать"),
                                        QStringLiteral("Атт.: задать"), QStringLiteral("Вкл.: задать"),
                                        QStringLiteral("Фаза: прочитано"), QStringLiteral("Атт.: прочитано"),
                                        QStringLiteral("Вкл.: прочитано")});
    m_codes->verticalHeader()->setVisible(false);
    m_codes->verticalHeader()->setDefaultSectionSize(34);
    m_codes->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_codes->setAlternatingRowColors(true);
    m_codes->setShowGrid(false);
    for (int row = 0; row < ps::kChannels; ++row) {
        auto* ch = new QTableWidgetItem(QString::number(row + 1));
        ch->setFlags(Qt::ItemIsEnabled);
        m_codes->setItem(row, 0, ch);
        for (int col : {1, 2, 3}) {
            auto* spin = new QSpinBox(m_codes);
            spin->setRange(0, col == 3 ? 1 : ps::kCodeMax);
            if (col == 3) spin->setValue(1);
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            m_codes->setCellWidget(row, col, spin);
        }
        for (int col : {4, 5, 6}) {
            auto* item = new QTableWidgetItem(QStringLiteral("—"));
            item->setFlags(Qt::ItemIsEnabled);
            m_codes->setItem(row, col, item);
        }
    }

    auto* fill = new QHBoxLayout;
    fill->setSpacing(ui::kInnerGap);
    m_fillPh = new QSpinBox(page);
    m_fillPh->setRange(0, ps::kCodeMax);
    m_fillPh->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_fillAtt = new QSpinBox(page);
    m_fillAtt->setRange(0, ps::kCodeMax);
    m_fillAtt->setButtonSymbols(QAbstractSpinBox::NoButtons);
    auto* fillBtn = new QPushButton(QStringLiteral("Заполнить все"), page);
    m_writeCodes = new QPushButton(QStringLiteral("Записать 16 каналов"), page);
    m_writeCodes->setObjectName(QStringLiteral("psWriteCodes"));
    m_readCodes = new QPushButton(QStringLiteral("Прочитать из микросхем"), page);
    m_readCodes->setObjectName(QStringLiteral("psReadCodes"));
    auto* saveBtn = new QPushButton(QStringLiteral("Сохранить набор…"), page);
    auto* loadBtn = new QPushButton(QStringLiteral("Загрузить набор…"), page);
    ui::setButtonRole(m_writeCodes, ui::ButtonRole::Primary);
    for (QPushButton* b : {fillBtn, m_readCodes, saveBtn, loadBtn}) {
        ui::setButtonRole(b, ui::ButtonRole::Secondary);
    }
    fill->addWidget(new QLabel(QStringLiteral("Фаза"), page));
    fill->addWidget(m_fillPh);
    fill->addWidget(new QLabel(QStringLiteral("Атт."), page));
    fill->addWidget(m_fillAtt);
    fill->addWidget(fillBtn);
    fill->addStretch(1);
    fill->addWidget(saveBtn);
    fill->addWidget(loadBtn);
    // Две строки кнопок: вкладка не растягивает «Модули» вширь.
    auto* write = new QHBoxLayout;
    write->setSpacing(ui::kInnerGap);
    write->addStretch(1);
    write->addWidget(m_readCodes);
    write->addWidget(m_writeCodes);
    layout->addWidget(m_codes, 1);
    layout->addLayout(fill);
    layout->addLayout(write);

    const QString filter = QStringLiteral("Коды решётки (*.json)");
    connect(saveBtn, &QPushButton::clicked, this, [this, filter] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранить набор кодов"),
                                                          QStringLiteral("codes.json"), filter);
        QString error;
        if (!path.isEmpty() && !saveCodes(path, &error)) {
            log(error);
        }
    });
    connect(loadBtn, &QPushButton::clicked, this, [this, filter] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Загрузить набор кодов"), QString(), filter);
        QString error;
        if (!path.isEmpty() && !loadCodes(path, &error)) {
            log(error);
        }
    });

    connect(fillBtn, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < ps::kChannels; ++row) {
            static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->setValue(m_fillPh->value());
            static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->setValue(m_fillAtt->value());
        }
    });
    connect(m_writeCodes, &QPushButton::clicked, this, &SettingsTab::writeCodes);
    connect(m_readCodes, &QPushButton::clicked, this, [this] { send(ps::msg::GetPhaseAtten); });
    m_serviceControls.insert(m_serviceControls.end(), {m_writeCodes, m_readCodes});
    return page;
}

QWidget* SettingsTab::buildBeamBox()
{
    auto* box = new QGroupBox(QStringLiteral("Луч"), this);
    box->setObjectName(QStringLiteral("beamBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setSpacing(ui::kInnerGap);
    m_beamAz = new QDoubleSpinBox(box);
    m_beamAz->setObjectName(QStringLiteral("psBeamAz"));
    m_beamAz->setRange(-ps::kAzimuthLimitDeg, ps::kAzimuthLimitDeg);
    m_beamAz->setDecimals(2);
    m_beamAz->setSuffix(QStringLiteral("°"));
    m_beamAz->setButtonSymbols(QAbstractSpinBox::NoButtons);
    auto* setAz = new QPushButton(QStringLiteral("Установить азимут"), box);
    setAz->setObjectName(QStringLiteral("psSetAzimuth"));
    m_beamId = new QSpinBox(box);
    m_beamId->setObjectName(QStringLiteral("psBeamId"));
    m_beamId->setRange(0, 1'000'000);
    m_beamId->setButtonSymbols(QAbstractSpinBox::NoButtons);
    auto* setBeam = new QPushButton(QStringLiteral("Установить луч"), box);
    setBeam->setObjectName(QStringLiteral("psSetBeam"));
    auto* getBeam = new QPushButton(QStringLiteral("Состояние луча"), box);
    getBeam->setObjectName(QStringLiteral("psGetBeam"));
    getBeam->setToolTip(QStringLiteral("get_beam: запросить состояние луча"));
    ui::setButtonRole(setAz, ui::ButtonRole::Primary);
    ui::setButtonRole(setBeam, ui::ButtonRole::Primary);
    ui::setButtonRole(getBeam, ui::ButtonRole::Secondary);
    m_beamState = new QLabel(QStringLiteral("луч: —"), box);
    m_beamState->setObjectName(QStringLiteral("psBeamState"));
    m_beamState->setProperty("role", QStringLiteral("hint"));
    m_beamState->setWordWrap(true);

    // Друг под другом: окошко узкое, стоит рядом с «Полным сканированием».
    layout->addWidget(ui::captionLabel(QStringLiteral("Азимут луча"), box));
    layout->addWidget(m_beamAz);
    layout->addWidget(setAz);
    layout->addWidget(ui::captionLabel(QStringLiteral("Луч из таблицы"), box));
    layout->addWidget(m_beamId);
    layout->addWidget(setBeam);
    layout->addWidget(getBeam);
    layout->addWidget(m_beamState);
    layout->addStretch(1);

    connect(setAz, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetBeamAzimuth, json{{"az", m_beamAz->value()}}); });
    connect(setBeam, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetBeam, json{{"beam", m_beamId->value()}}); });
    connect(getBeam, &QPushButton::clicked, this, [this] { send(ps::msg::GetBeam); });
    m_serviceControls.insert(m_serviceControls.end(), {setAz, setBeam, getBeam});
    return box;
}

void SettingsTab::startSelfTest()
{
    if (m_session->send(ps::msg::SetStartTest, json::object()) >= 0) {
        log(QStringLiteral("Самопроверка запущена, ждём ответа PS"));
    }
}

void SettingsTab::resetBoard()
{
    if (m_session->send(ps::msg::SetReset, json::object()) >= 0) {
        log(QStringLiteral("Команда перезапуска отправлена"));
    }
}

void SettingsTab::send(std::string_view msgType, const json& payload)
{
    m_session->send(msgType, payload);
}

void SettingsTab::writeCodes()
{
    std::vector<ps::ChannelCodes> codes;
    for (int row = 0; row < ps::kChannels; ++row) {
        codes.push_back({row + 1, static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->value(),
                         static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->value(),
                         static_cast<QSpinBox*>(m_codes->cellWidget(row, 3))->value()});
    }
    json payload;
    std::string error;
    if (!ps::channelsToJson(codes, payload, &error)) {
        log(QString::fromStdString(error));
        return;
    }
    send(ps::msg::SetPhaseAtten, payload);
}

bool SettingsTab::saveCodes(const QString& path, QString* error) const
{
    std::vector<ps::ChannelCodes> codes;
    for (int row = 0; row < ps::kChannels; ++row) {
        codes.push_back({row + 1, static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->value(),
                         static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->value(),
                         static_cast<QSpinBox*>(m_codes->cellWidget(row, 3))->value()});
    }
    json payload;
    std::string why;
    if (!ps::channelsToJson(codes, payload, &why)) {
        if (error) {
            *error = QString::fromStdString(why);
        }
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Набор кодов не сохранён: %1").arg(f.errorString());
        }
        return false;
    }
    f.write(QByteArray::fromStdString(payload.dump(2) + "\n"));
    return true;
}

bool SettingsTab::loadCodes(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Набор кодов не прочитан: %1").arg(f.errorString());
        }
        return false;
    }
    // Набор на 16 каналов — сотни байт; больше — не наш файл.
    const QByteArray bytes = f.read(64 * 1024);
    const json payload = json::parse(bytes.toStdString(), nullptr, false);
    std::vector<ps::ChannelCodes> codes;
    std::string why = "файл не JSON";
    if (payload.is_discarded() || !ps::channelsFromJson(payload, codes, &why)) {
        if (error) {
            *error = QStringLiteral("Набор кодов отклонён: %1").arg(QString::fromStdString(why));
        }
        return false;
    }
    for (const auto& c : codes) {
        static_cast<QSpinBox*>(m_codes->cellWidget(c.n - 1, 1))->setValue(c.ph);
        static_cast<QSpinBox*>(m_codes->cellWidget(c.n - 1, 2))->setValue(c.att);
        static_cast<QSpinBox*>(m_codes->cellWidget(c.n - 1, 3))->setValue(c.en);
    }
    log(QStringLiteral("Набор кодов загружен: %1").arg(path));
    return true;
}

void SettingsTab::showCodes(const std::vector<ps::ChannelCodes>& codes)
{
    for (const auto& c : codes) {
        const int row = c.n - 1;
        const int wantPh = static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->value();
        const int wantAtt = static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->value();
        const int wantEn = static_cast<QSpinBox*>(m_codes->cellWidget(row, 3))->value();
        auto* ph = m_codes->item(row, 4);
        auto* att = m_codes->item(row, 5);
        auto* en = m_codes->item(row, 6);
        ph->setText(QString::number(c.ph));
        att->setText(QString::number(c.att));
        en->setText(QString::number(c.en));
        // Расхождение с заданным — текстом, а не только цветом (ТЗ т. 5.2).
        if (c.ph != wantPh) {
            ph->setText(QStringLiteral("%1 ≠ %2").arg(c.ph).arg(wantPh));
        }
        if (c.att != wantAtt) {
            att->setText(QStringLiteral("%1 ≠ %2").arg(c.att).arg(wantAtt));
        }
        if (c.en != wantEn) {
            en->setText(QStringLiteral("%1 ≠ %2").arg(c.en).arg(wantEn));
        }
    }
}

void SettingsTab::onResponse(const ps::Envelope& r, ps::Status status)
{
    const QString type = qs(r.msg_type);
    if (status != ps::Status::Ok) {
        return;  // отказ PS уже в журнале сообщений сессии
    }
    std::string error;
    if (r.msg_type == ps::msg::SetStartTest) {
        log(QStringLiteral("Самопроверка завершена. Результаты проверок — в техническом состоянии изделия"));
        send(ps::msg::GetState);
    } else if (r.msg_type == ps::msg::SetReset) {
        log(QStringLiteral("Изделие перезапускается. Программа переподключится сама, когда PS снова будет доступна"));
    } else if (r.msg_type == ps::msg::SetPhaseAtten || r.msg_type == ps::msg::GetPhaseAtten) {
        std::vector<ps::ChannelCodes> codes;
        if (ps::channelsFromJson(r.payload, codes, &error)) {
            showCodes(codes);
        }
    } else if (r.msg_type == ps::msg::SetBeamAzimuth || r.msg_type == ps::msg::SetBeam ||
               r.msg_type == ps::msg::GetBeam) {
        if (r.msg_type != ps::msg::GetBeam) {
            send(ps::msg::GetBeam);
        } else {
            ps::BeamState b;
            if (ps::BeamState::fromJson(r.payload, b, &error)) {
                switch (b.src) {
                case ps::BeamSource::Azimuth: m_beamState->setText(QStringLiteral("луч: азимут %1°").arg(b.az)); break;
                case ps::BeamSource::Table: m_beamState->setText(QStringLiteral("луч: № %1 из таблицы").arg(b.beam)); break;
                case ps::BeamSource::Codes: m_beamState->setText(QStringLiteral("луч: заданы коды каналов")); break;
                case ps::BeamSource::None: m_beamState->setText(QStringLiteral("луч: не задавался")); break;
                }
            }
        }
    }
    if (!error.empty()) {
        log(QStringLiteral("%1: ответ не разобран — %2").arg(type, QString::fromStdString(error)));
    }
}

void SettingsTab::onTelemetry()
{
    const PsLink* link = m_session->link();
    m_tlmCounters->setText(QStringLiteral("10-байтных пакетов: принято %1, отброшено %2 (потери без счётчика не определяются)")
                               .arg(link->telemetryPackets())
                               .arg(link->telemetryRejected()));
    const auto& tlm = m_session->telemetry();
    if (!tlm) {
        return;
    }
    const ps::Telemetry& t = *tlm;
    std::vector<std::pair<QString, QString>> rows;
    if (t.hasMode) {
        rows.emplace_back(QStringLiteral("Режим"), QString::fromStdString(ps::modeRu(t.mode)));
    }
    if (t.cpuLoad) {
        rows.emplace_back(QStringLiteral("Загрузка процессора"), QStringLiteral("%1 %").arg(*t.cpuLoad, 0, 'f', 1));
    }
    for (std::size_t i = 0; i < t.cpuCores.size(); ++i) {
        rows.emplace_back(QStringLiteral("Ядро %1").arg(i), QStringLiteral("%1 %").arg(t.cpuCores[i], 0, 'f', 1));
    }
    if (!t.state.empty()) {
        rows.emplace_back(QStringLiteral("Состояние"), QString::fromStdString(ps::stateRu(t.state)));
    }
    if (t.uptime > 0) {
        rows.emplace_back(QStringLiteral("Время работы"), QStringLiteral("%1 с").arg(t.uptime));
    }
    for (const auto& [id, h] : t.devices) {
        rows.emplace_back(QStringLiteral("Проверка: %1").arg(QString::fromStdString(ps::selfTestRu(id))),
                          qs(ps::toString(h)));
    }
    for (const auto& [id, v] : t.temperature) {
        rows.emplace_back(QStringLiteral("Температура %1").arg(QString::fromStdString(id)),
                          QStringLiteral("%1 °C").arg(v, 0, 'f', 1));
    }
    for (const auto& [id, v] : t.power) {
        rows.emplace_back(QStringLiteral("Питание %1").arg(QString::fromStdString(id)),
                          QStringLiteral("%1 В").arg(v, 0, 'f', 2));
    }
    m_tlmTable->setRowCount(static_cast<int>(rows.size()));
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        m_tlmTable->setItem(i, 0, new QTableWidgetItem(rows[static_cast<std::size_t>(i)].first));
        m_tlmTable->setItem(i, 1, new QTableWidgetItem(rows[static_cast<std::size_t>(i)].second));
    }
}

void SettingsTab::refreshMode()
{
    QString text = QStringLiteral("режим: —");
    if (m_session->modeKnown()) {
        text = QStringLiteral("режим: %1").arg(QString::fromStdString(ps::modeRu(m_session->mode())));
        if (!m_session->state().isEmpty()) {
            text += QStringLiteral(", состояние: %1")
                        .arg(QString::fromStdString(ps::stateRu(m_session->state().toStdString())));
        }
    }
    m_modeLabel->setText(text);
    ui::setTone(m_modeLabel, m_session->modeKnown() ? (m_session->mode() == ps::Mode::Service ? ui::Tone::Warn : ui::Tone::Ok)
                                                    : ui::Tone::Neutral);
    refreshEnabled();
}

void SettingsTab::refreshEnabled()
{
    const bool ready = m_session->manualAllowed();
    const bool known = m_session->modeKnown();
    const ps::Mode mode = m_session->mode();
    const bool service = ready && known && mode == ps::Mode::Service;
    m_modulesPage->setAllowed(service);
    m_diagnostics->setAllowed(service);
    m_osd->setAllowed(ready, service);
    // set_start_test и set_reset — только в IDLE/READY (v8 п. 3.2).
    const QString state = m_session->state().toUpper();
    const bool idleReady = ready && known && mode == ps::Mode::Idle && state != QStringLiteral("INIT") &&
                           state != QStringLiteral("SELF_TEST");
    m_selfTest->setEnabled(idleReady);
    m_reset->setEnabled(idleReady);
    m_toService->setEnabled(ready && (!known || mode == ps::Mode::Idle));
    m_toOperational->setEnabled(ready && (!known || mode == ps::Mode::Idle || mode == ps::Mode::FullScanFreq));
    m_toIdle->setEnabled(ready && known && mode != ps::Mode::Idle);
    m_getState->setEnabled(ready);
    for (QWidget* w : m_serviceControls) {
        w->setEnabled(service);
    }
}

void SettingsTab::log(const QString& text)
{
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), text));
}

QWidget* SettingsTab::buildConnectionBox()
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
    // Кнопки друг под другом: окошко узкое, рядом с ним в ряд ещё три.
    auto* buttons = new QVBoxLayout;
    buttons->setSpacing(ui::kInnerGap);
    buttons->addWidget(m_connect);
    buttons->addWidget(m_findByMac);

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

    connect(m_connect, &QPushButton::clicked, this, &SettingsTab::onMainButton);
    // Enter в любом поле — подключиться с тем, что введено (и повторить попытку, если она уже идёт).
    for (QLineEdit* edit : {m_host, m_mac}) {
        connect(edit, &QLineEdit::returnPressed, this, &SettingsTab::connectNow);
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

QWidget* SettingsTab::buildTechBox()
{
    auto* box = new QGroupBox(QStringLiteral("Тех. состояние"), this);
    box->setObjectName(QStringLiteral("techBox"));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    m_techBody = new QWidget(box);
    auto* bodyLayout = new QVBoxLayout(m_techBody);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_techBody, 1);
    // Трансивер AD9361, ПЛИС, РЧ-модули. Пока параметров платы нет — прямо в окошке;
    // setTechPage переносит их вкладкой «Состояние» к параметрам платы.
    m_diagnostics = new PsDiagnosticsPage(m_session, m_techBody);
    bodyLayout->addWidget(m_diagnostics, 1);
    return box;
}

void SettingsTab::setTechPage(QWidget* page)
{
    page->setParent(m_techBody);
    if (auto* board = qobject_cast<BoardParametersPage*>(page)) {
        m_techBody->layout()->removeWidget(m_diagnostics);
        board->insertResultTab(2, m_diagnostics, QStringLiteral("Состояние"));
    }
    m_techBody->layout()->addWidget(page);
}

void SettingsTab::addTechSection(QWidget* section)
{
    section->setParent(m_techBody);
    m_techBody->layout()->addWidget(section);
}

void SettingsTab::openJournal()
{
    m_journal->open();
    m_journal->raise();
}

QString SettingsTab::macText() const
{
    return MacResolver::formatMac(m_mac->text().trimmed());
}

void SettingsTab::connectNow()
{
    if (m_session->link()->isReady() || m_session->seriesActive()) {
        return;
    }
    m_session->connectTo(m_host->text(), static_cast<quint16>(m_port->value()),
                         static_cast<quint16>(m_tlmPort->value()), m_mac->text());
}

void SettingsTab::onMainButton()
{
    // Связь есть — отключиться; идёт попытка или поиск — отменить; иначе подключиться.
    if (m_session->link()->isReady() || m_session->busy()) {
        m_session->disconnectFromPs();
        return;
    }
    connectNow();
}

void SettingsTab::refreshConnection()
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

void SettingsTab::refreshCounters()
{
    const PsLink* link = m_session->link();
    m_tcpOut->setValue(QString::number(m_tcpOutCount));
    m_tcpIn->setValue(QString::number(m_tcpInCount));
    m_udpIn->setValue(QString::number(link->telemetryPackets()));
    m_udpLost->setValue(QString::number(link->telemetryRejected()));
    m_udpLost->setTone(link->telemetryRejected() > 0 ? ui::Tone::Warn : ui::Tone::Neutral);
    m_messages->setValue(QString::number(m_messageCount));
    m_messages->setTone(m_messageCount > 0 ? ui::Tone::Warn : ui::Tone::Neutral);
}
