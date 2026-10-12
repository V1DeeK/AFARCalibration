#include "PsTab.h"

#include "PsSimulator.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
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

QString hzText(double hz)
{
    return QStringLiteral("%1 МГц").arg(hz / 1e6, 0, 'f', 6);
}

QString fieldText(ps::Rs485FieldKind kind, double v)
{
    switch (kind) {
    case ps::Rs485FieldKind::Flag: return v != 0.0 ? QStringLiteral("1 (вкл)") : QStringLiteral("0 (выкл)");
    case ps::Rs485FieldKind::PowerDbm: return QStringLiteral("%1 дБм").arg(v, 0, 'f', 0);
    case ps::Rs485FieldKind::FrequencyHz: return hzText(v);
    case ps::Rs485FieldKind::Att31:
    case ps::Rs485FieldKind::Att15: return QStringLiteral("%1 дБ").arg(v, 0, 'f', 1);
    }
    return QString::number(v);
}

}  // namespace

PsTab::PsTab(QWidget* parent)
    : QWidget(parent)
    , m_link(new PsLink(this))
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    auto* top = new QHBoxLayout;
    top->addWidget(buildConnectionBox(), 3);
    top->addWidget(buildModeBox(), 2);
    root->addLayout(top);

    auto* middle = new QHBoxLayout;
    m_pages = new QTabWidget(this);
    m_pages->addTab(buildCodesPage(), QStringLiteral("Коды решётки"));
    m_pages->addTab(buildAdcPage(), QStringLiteral("АЦП"));
    m_pages->addTab(buildBeamPage(), QStringLiteral("Луч"));
    m_pages->addTab(buildModulesPage(), QStringLiteral("Модули RS-485"));
    m_pages->addTab(buildRawPage(), QStringLiteral("RS-485 HEX"));
    middle->addWidget(m_pages, 3);
    middle->addWidget(buildTelemetryBox(), 2);
    root->addLayout(middle, 1);

    auto* logBox = new QGroupBox(QStringLiteral("Журнал обмена"), this);
    auto* logLayout = new QVBoxLayout(logBox);
    m_log = new QPlainTextEdit(logBox);
    m_log->setObjectName(QStringLiteral("psLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setMinimumHeight(90);
    m_logTelemetry = new QCheckBox(QStringLiteral("Показывать датаграммы телеметрии"), logBox);
    logLayout->addWidget(m_log);
    logLayout->addWidget(m_logTelemetry);
    root->addWidget(logBox);

    connect(m_link, &PsLink::stateChanged, this, &PsTab::onLinkStateChanged);
    connect(m_link, &PsLink::responseReceived, this, &PsTab::onResponse);
    connect(m_link, &PsLink::telemetryReceived, this, &PsTab::onTelemetry);
    connect(m_link, &PsLink::frameLogged, this, &PsTab::onFrame);
    connect(m_link, &PsLink::errorOccurred, this,
            [this](const QString& message) { log(QStringLiteral("Ошибка: %1").arg(message)); });
    connect(m_link, &PsLink::commandTimedOut, this, [this](const QString& type, int count) {
        log(QStringLiteral("Нет ответа на %1 #%2").arg(type).arg(count));
    });

    onLinkStateChanged(PsLink::State::Disconnected);
}

PsTab::~PsTab() = default;

QWidget* PsTab::buildConnectionBox()
{
    auto* box = new QGroupBox(QStringLiteral("Связь с PS"), this);
    auto* grid = new QGridLayout(box);
    m_host = new QLineEdit(box);
    m_host->setObjectName(QStringLiteral("psHost"));
    m_host->setPlaceholderText(QStringLiteral("адрес PS из профиля стенда"));
    m_port = new QSpinBox(box);
    m_port->setRange(1, 65535);
    m_port->setValue(ps::kDefaultControlPort);
    m_tlmPort = new QSpinBox(box);
    m_tlmPort->setRange(1, 65535);
    m_tlmPort->setValue(ps::kDefaultTelemetryPort);
    m_useSim = new QCheckBox(QStringLiteral("Имитатор PS (без стенда)"), box);
    m_useSim->setObjectName(QStringLiteral("psUseSimulator"));
    m_connect = new QPushButton(QStringLiteral("Подключить"), box);
    m_connect->setObjectName(QStringLiteral("psConnect"));
    m_linkState = new QLabel(box);
    m_linkState->setObjectName(QStringLiteral("psLinkState"));
    m_linkState->setWordWrap(true);

    grid->addWidget(new QLabel(QStringLiteral("Адрес"), box), 0, 0);
    grid->addWidget(m_host, 0, 1, 1, 3);
    grid->addWidget(new QLabel(QStringLiteral("TCP"), box), 1, 0);
    grid->addWidget(m_port, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("UDP телеметрии"), box), 1, 2);
    grid->addWidget(m_tlmPort, 1, 3);
    grid->addWidget(m_useSim, 2, 0, 1, 2);
    grid->addWidget(m_connect, 2, 3);
    grid->addWidget(m_linkState, 3, 0, 1, 4);

    connect(m_connect, &QPushButton::clicked, this, &PsTab::onConnectClicked);
    connect(m_useSim, &QCheckBox::toggled, this, [this](bool on) {
        m_host->setEnabled(!on);
        m_port->setEnabled(!on);
    });
    return box;
}

QWidget* PsTab::buildModeBox()
{
    auto* box = new QGroupBox(QStringLiteral("Режим изделия"), this);
    auto* layout = new QVBoxLayout(box);
    m_modeLabel = new QLabel(QStringLiteral("режим: —"), box);
    m_modeLabel->setObjectName(QStringLiteral("psMode"));
    m_toService = new QPushButton(QStringLiteral("Перейти в сервисный"), box);
    m_toService->setObjectName(QStringLiteral("psToService"));
    m_toIdle = new QPushButton(QStringLiteral("Вернуть в ожидание"), box);
    m_toIdle->setObjectName(QStringLiteral("psToIdle"));
    m_getState = new QPushButton(QStringLiteral("Запросить состояние"), box);
    auto* hint = new QLabel(
        QStringLiteral("В сервисный режим PS переходит только из ожидания (READY). "
                       "При выходе из него PS сам восстанавливает рабочую конфигурацию."),
        box);
    hint->setWordWrap(true);
    layout->addWidget(m_modeLabel);
    layout->addWidget(m_toService);
    layout->addWidget(m_toIdle);
    layout->addWidget(m_getState);
    layout->addWidget(hint);
    layout->addStretch(1);

    connect(m_toService, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetMode, json{{"mode", "SERVICE"}}); });
    connect(m_toIdle, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetMode, json{{"mode", "IDLE"}}); });
    connect(m_getState, &QPushButton::clicked, this, [this] { send(ps::msg::GetState); });
    return box;
}

QWidget* PsTab::buildTelemetryBox()
{
    auto* box = new QGroupBox(QStringLiteral("Телеметрия"), this);
    auto* layout = new QVBoxLayout(box);
    m_tlmCounters = new QLabel(QStringLiteral("датаграмм нет"), box);
    m_tlmCounters->setObjectName(QStringLiteral("psTlmCounters"));
    m_tlmTable = new QTableWidget(0, 2, box);
    m_tlmTable->setObjectName(QStringLiteral("psTlmTable"));
    m_tlmTable->setHorizontalHeaderLabels({QStringLiteral("Параметр"), QStringLiteral("Значение")});
    m_tlmTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tlmTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tlmTable->verticalHeader()->setVisible(false);
    m_tlmTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_tlmCounters);
    layout->addWidget(m_tlmTable, 1);
    return box;
}

QWidget* PsTab::buildCodesPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    m_codes = new QTableWidget(ps::kChannels, 5, page);
    m_codes->setObjectName(QStringLiteral("psCodes"));
    m_codes->setHorizontalHeaderLabels({QStringLiteral("Канал"), QStringLiteral("Фаза: задать"),
                                        QStringLiteral("Атт.: задать"), QStringLiteral("Фаза: прочитано"),
                                        QStringLiteral("Атт.: прочитано")});
    m_codes->verticalHeader()->setVisible(false);
    m_codes->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int row = 0; row < ps::kChannels; ++row) {
        auto* ch = new QTableWidgetItem(QString::number(row + 1));
        ch->setFlags(Qt::ItemIsEnabled);
        m_codes->setItem(row, 0, ch);
        for (int col : {1, 2}) {
            auto* spin = new QSpinBox(m_codes);
            spin->setRange(0, ps::kCodeMax);
            m_codes->setCellWidget(row, col, spin);
        }
        for (int col : {3, 4}) {
            auto* item = new QTableWidgetItem(QStringLiteral("—"));
            item->setFlags(Qt::ItemIsEnabled);
            m_codes->setItem(row, col, item);
        }
    }

    auto* fill = new QHBoxLayout;
    m_fillPh = new QSpinBox(page);
    m_fillPh->setRange(0, ps::kCodeMax);
    m_fillAtt = new QSpinBox(page);
    m_fillAtt->setRange(0, ps::kCodeMax);
    auto* fillBtn = new QPushButton(QStringLiteral("Заполнить все"), page);
    m_writeCodes = new QPushButton(QStringLiteral("Записать 16 каналов"), page);
    m_writeCodes->setObjectName(QStringLiteral("psWriteCodes"));
    m_readCodes = new QPushButton(QStringLiteral("Прочитать из микросхем"), page);
    m_readCodes->setObjectName(QStringLiteral("psReadCodes"));
    fill->addWidget(new QLabel(QStringLiteral("Фаза"), page));
    fill->addWidget(m_fillPh);
    fill->addWidget(new QLabel(QStringLiteral("Атт."), page));
    fill->addWidget(m_fillAtt);
    fill->addWidget(fillBtn);
    fill->addStretch(1);
    fill->addWidget(m_writeCodes);
    fill->addWidget(m_readCodes);
    layout->addWidget(m_codes, 1);
    layout->addLayout(fill);

    connect(fillBtn, &QPushButton::clicked, this, [this] {
        for (int row = 0; row < ps::kChannels; ++row) {
            static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->setValue(m_fillPh->value());
            static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->setValue(m_fillAtt->value());
        }
    });
    connect(m_writeCodes, &QPushButton::clicked, this, &PsTab::writeCodes);
    connect(m_readCodes, &QPushButton::clicked, this, [this] { send(ps::msg::GetPhaseAtten); });
    m_serviceControls.insert(m_serviceControls.end(), {m_writeCodes, m_readCodes});
    return page;
}

QWidget* PsTab::buildAdcPage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    const auto mhz = [page](double value, double max) {
        auto* s = new QDoubleSpinBox(page);
        s->setRange(0.0, max);
        s->setDecimals(6);
        s->setSuffix(QStringLiteral(" МГц"));
        s->setValue(value);
        return s;
    };
    m_adcLoMhz = mhz(5800.0, 100000.0);
    m_adcLoMhz->setObjectName(QStringLiteral("psAdcLo"));
    m_adcBwMhz = mhz(20.0, 100000.0);
    m_adcFsMhz = mhz(30.72, 100000.0);
    m_adcGain = new QDoubleSpinBox(page);
    m_adcGain->setRange(-100.0, 100.0);
    m_adcGain->setDecimals(1);
    m_adcGain->setSuffix(QStringLiteral(" дБ"));
    m_adcGain->setValue(40.0);
    m_adcActual = new QLabel(QStringLiteral("установлено: —"), page);
    m_adcActual->setWordWrap(true);
    auto* apply = new QPushButton(QStringLiteral("Применить"), page);
    apply->setObjectName(QStringLiteral("psAdcApply"));
    auto* read = new QPushButton(QStringLiteral("Прочитать"), page);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(apply);
    buttons->addWidget(read);
    buttons->addStretch(1);

    form->addRow(QStringLiteral("Гетеродин приёмника lo"), m_adcLoMhz);
    form->addRow(QStringLiteral("Полоса RF bw"), m_adcBwMhz);
    form->addRow(QStringLiteral("Частота дискретизации fs"), m_adcFsMhz);
    form->addRow(QStringLiteral("Усиление gain (ручное)"), m_adcGain);
    form->addRow(buttons);
    form->addRow(m_adcActual);
    form->addRow(new QLabel(QStringLiteral("Рабочий диапазон изделия 4,9–6,0 ГГц; частоту вне "
                                           "диапазона отклоняет PS."),
                            page));

    connect(apply, &QPushButton::clicked, this, &PsTab::applyAdc);
    connect(read, &QPushButton::clicked, this, [this] { send(ps::msg::GetAdc); });
    m_serviceControls.insert(m_serviceControls.end(), {apply, read});
    return page;
}

QWidget* PsTab::buildBeamPage()
{
    auto* page = new QWidget(this);
    auto* form = new QFormLayout(page);
    m_beamAz = new QDoubleSpinBox(page);
    m_beamAz->setRange(-ps::kAzimuthLimitDeg, ps::kAzimuthLimitDeg);
    m_beamAz->setDecimals(2);
    m_beamAz->setSuffix(QStringLiteral("°"));
    auto* setAz = new QPushButton(QStringLiteral("Установить азимут"), page);
    m_beamId = new QSpinBox(page);
    m_beamId->setRange(0, 1'000'000);
    auto* setBeam = new QPushButton(QStringLiteral("Установить луч"), page);
    auto* getBeam = new QPushButton(QStringLiteral("Запросить состояние луча"), page);
    m_beamState = new QLabel(QStringLiteral("луч: —"), page);

    auto* azRow = new QHBoxLayout;
    azRow->addWidget(m_beamAz);
    azRow->addWidget(setAz);
    auto* idRow = new QHBoxLayout;
    idRow->addWidget(m_beamId);
    idRow->addWidget(setBeam);
    form->addRow(QStringLiteral("Азимут луча"), azRow);
    form->addRow(QStringLiteral("Луч из таблицы"), idRow);
    form->addRow(getBeam);
    form->addRow(m_beamState);

    connect(setAz, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetBeamAzimuth, json{{"az", m_beamAz->value()}}); });
    connect(setBeam, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetBeam, json{{"beam", m_beamId->value()}}); });
    connect(getBeam, &QPushButton::clicked, this, [this] { send(ps::msg::GetBeam); });
    m_serviceControls.insert(m_serviceControls.end(), {setAz, setBeam, getBeam});
    return page;
}

QWidget* PsTab::buildModulesPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* head = new QHBoxLayout;
    m_module = new QComboBox(page);
    m_module->setObjectName(QStringLiteral("psModule"));
    for (int addr = 1; addr <= 4; ++addr) {
        const auto m = static_cast<ps::Rs485Module>(addr);
        m_module->addItem(QStringLiteral("0x%1 — %2")
                              .arg(addr, 2, 16, QLatin1Char('0'))
                              .arg(QString::fromStdString(ps::rs485ModuleRu(m))),
                          addr);
    }
    m_moduleStatus = new QLabel(QStringLiteral("Temp: —, Lock: —"), page);
    head->addWidget(m_module, 1);
    head->addWidget(m_moduleStatus);

    m_moduleTable = new QTableWidget(0, 4, page);
    m_moduleTable->setObjectName(QStringLiteral("psModuleTable"));
    m_moduleTable->setHorizontalHeaderLabels({QStringLiteral("Передать"), QStringLiteral("Поле"),
                                              QStringLiteral("Задать"), QStringLiteral("Состояние")});
    m_moduleTable->verticalHeader()->setVisible(false);
    m_moduleTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_moduleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_moduleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_moduleTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    auto* buttons = new QHBoxLayout;
    auto* write = new QPushButton(QStringLiteral("Записать отмеченные"), page);
    write->setObjectName(QStringLiteral("psModuleWrite"));
    auto* status = new QPushButton(QStringLiteral("Запросить статус"), page);
    status->setObjectName(QStringLiteral("psModuleStatus"));
    auto* note = new QLabel(QStringLiteral("Команды rs485_set / rs485_status — предложение контракта, "
                                           "ждут согласования с разработчиком PS."),
                            page);
    note->setWordWrap(true);
    buttons->addWidget(write);
    buttons->addWidget(status);
    buttons->addStretch(1);

    layout->addLayout(head);
    layout->addWidget(m_moduleTable, 1);
    layout->addLayout(buttons);
    layout->addWidget(note);

    connect(m_module, &QComboBox::currentIndexChanged, this, [this](int) { rebuildModuleForm(); });
    connect(write, &QPushButton::clicked, this, &PsTab::writeModule);
    connect(status, &QPushButton::clicked, this, &PsTab::requestModuleStatus);
    m_serviceControls.insert(m_serviceControls.end(), {write, status});
    rebuildModuleForm();
    return page;
}

QWidget* PsTab::buildRawPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    m_rawHex = new QLineEdit(page);
    m_rawHex->setObjectName(QStringLiteral("psRawHex"));
    m_rawHex->setPlaceholderText(QStringLiteral("например 0101020F1E…"));
    auto* sendBtn = new QPushButton(QStringLiteral("Передать в линию"), page);
    auto* note = new QLabel(QStringLiteral("Байты уходят в линию RS-485 без изменений, включая CRC16. "
                                           "Ответ устройства PS пока не возвращает."),
                            page);
    note->setWordWrap(true);
    auto* row = new QHBoxLayout;
    row->addWidget(m_rawHex, 1);
    row->addWidget(sendBtn);
    layout->addLayout(row);
    layout->addWidget(note);
    layout->addStretch(1);

    connect(sendBtn, &QPushButton::clicked, this, [this] {
        const QString hex = m_rawHex->text().remove(QLatin1Char(' ')).toUpper();
        if (!ps::hexBytesValid(hex.toStdString())) {
            log(QStringLiteral("HEX-строка должна быть чётной длины и только из 0–9, A–F"));
            return;
        }
        send(ps::msg::Rs485Send, json{{"data", hex.toStdString()}});
    });
    m_serviceControls.push_back(sendBtn);
    return page;
}

void PsTab::rebuildModuleForm()
{
    m_moduleRows.clear();
    m_moduleTable->setRowCount(0);
    m_moduleStatus->setText(QStringLiteral("Temp: —, Lock: —"));
    const auto module = static_cast<ps::Rs485Module>(m_module->currentData().toInt());
    const auto& fields = ps::rs485Fields(module);
    m_moduleTable->setRowCount(static_cast<int>(fields.size()));
    int row = 0;
    for (const auto& f : fields) {
        ModuleRow r;
        r.kind = f.kind;
        r.send = new QCheckBox(m_moduleTable);
        auto* name = new QTableWidgetItem(QStringLiteral("%1 (%2)").arg(qs(f.titleRu), qs(f.key)));
        name->setFlags(Qt::ItemIsEnabled);
        switch (f.kind) {
        case ps::Rs485FieldKind::Flag: {
            auto* box = new QCheckBox(QStringLiteral("вкл"), m_moduleTable);
            r.editor = box;
            connect(box, &QCheckBox::toggled, r.send, [send = r.send] { send->setChecked(true); });
            break;
        }
        case ps::Rs485FieldKind::PowerDbm: {
            auto* combo = new QComboBox(m_moduleTable);
            for (int dbm : {-4, -1, 2, 5}) {
                combo->addItem(QStringLiteral("%1 дБм").arg(dbm), dbm);
            }
            r.editor = combo;
            connect(combo, &QComboBox::currentIndexChanged, r.send,
                    [send = r.send] { send->setChecked(true); });
            break;
        }
        case ps::Rs485FieldKind::FrequencyHz:
        case ps::Rs485FieldKind::Att31:
        case ps::Rs485FieldKind::Att15: {
            auto* spin = new QDoubleSpinBox(m_moduleTable);
            if (f.kind == ps::Rs485FieldKind::FrequencyHz) {
                spin->setRange(0.0, 7000.0);
                spin->setDecimals(6);
                spin->setSuffix(QStringLiteral(" МГц"));
            } else {
                spin->setRange(0.0, f.kind == ps::Rs485FieldKind::Att31 ? 31.5 : 15.5);
                spin->setDecimals(1);
                spin->setSingleStep(0.5);
                spin->setSuffix(QStringLiteral(" дБ"));
            }
            r.editor = spin;
            connect(spin, &QDoubleSpinBox::valueChanged, r.send,
                    [send = r.send] { send->setChecked(true); });
            break;
        }
        }
        r.actual = new QLabel(QStringLiteral("—"), m_moduleTable);
        m_moduleTable->setCellWidget(row, 0, r.send);
        m_moduleTable->setItem(row, 1, name);
        m_moduleTable->setCellWidget(row, 2, r.editor);
        m_moduleTable->setCellWidget(row, 3, r.actual);
        m_moduleRows.emplace(std::string(f.key), r);
        ++row;
    }
}

void PsTab::onConnectClicked()
{
    if (m_link->state() != PsLink::State::Disconnected) {
        m_link->disconnectFromPs();
        if (m_sim) {
            m_sim->close();
        }
        return;
    }
    QString host = m_host->text().trimmed();
    quint16 port = static_cast<quint16>(m_port->value());
    if (m_useSim->isChecked()) {
        if (!m_sim) {
            m_sim = new PsSimulator(this);
        }
        if (!m_sim->isListening() && !m_sim->listen()) {
            log(QStringLiteral("Имитатор PS не смог открыть порт"));
            return;
        }
        host = QStringLiteral("127.0.0.1");
        port = m_sim->port();
        log(QStringLiteral("Имитатор PS запущен на 127.0.0.1:%1 — ДЕМО, НЕ ДЛЯ МЕТРОЛОГИИ").arg(port));
    }
    if (host.isEmpty()) {
        log(QStringLiteral("Укажите адрес PS"));
        return;
    }
    m_link->connectToPs(host, port, static_cast<quint16>(m_tlmPort->value()));
}

void PsTab::onLinkStateChanged(PsLink::State state)
{
    QString text;
    bool ok = false;
    switch (state) {
    case PsLink::State::Disconnected:
        text = m_link->lastError().isEmpty()
                   ? QStringLiteral("нет связи")
                   : QStringLiteral("нет связи: %1").arg(m_link->lastError());
        m_modeKnown = false;
        m_modeLabel->setText(QStringLiteral("режим: —"));
        break;
    case PsLink::State::Connecting:
        text = QStringLiteral("подключение к %1:%2…").arg(m_link->host()).arg(m_link->port());
        break;
    case PsLink::State::Handshaking:
        text = QStringLiteral("рукопожатие hello…");
        break;
    case PsLink::State::Ready:
        ok = true;
        text = QStringLiteral("на связи %1:%2, протокол %3, телеметрия UDP %4")
                   .arg(m_link->host())
                   .arg(m_link->port())
                   .arg(m_link->peerVersion())
                   .arg(m_link->telemetryPort());
        send(ps::msg::GetState);
        break;
    }
    m_linkState->setText(text);
    m_connect->setText(state == PsLink::State::Disconnected ? QStringLiteral("Подключить")
                                                            : QStringLiteral("Отключить"));
    m_useSim->setEnabled(state == PsLink::State::Disconnected);
    emit summaryChanged(ok ? QStringLiteral("PS: OK") : QStringLiteral("PS: нет связи"), ok);
    refreshEnabled();
}

void PsTab::send(std::string_view msgType, const json& payload)
{
    if (m_seriesActive) {
        log(QStringLiteral("Идёт серия: ручные команды PS заблокированы"));
        return;
    }
    if (m_link->sendCommand(msgType, payload) < 0) {
        log(QStringLiteral("Нет связи с PS: %1 не отправлена").arg(qs(msgType)));
    }
}

void PsTab::writeCodes()
{
    std::vector<ps::ChannelCodes> codes;
    for (int row = 0; row < ps::kChannels; ++row) {
        codes.push_back({row + 1, static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->value(),
                         static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->value()});
    }
    json payload;
    std::string error;
    if (!ps::channelsToJson(codes, payload, &error)) {
        log(QString::fromStdString(error));
        return;
    }
    send(ps::msg::SetPhaseAtten, payload);
}

void PsTab::applyAdc()
{
    ps::AdcParams adc;
    adc.lo = std::llround(m_adcLoMhz->value() * 1e6);
    adc.bw = std::llround(m_adcBwMhz->value() * 1e6);
    adc.fs = std::llround(m_adcFsMhz->value() * 1e6);
    adc.gain = m_adcGain->value();
    std::string error;
    if (!adc.validate(&error)) {
        log(QString::fromStdString(error));
        return;
    }
    send(ps::msg::SetAdc, adc.toJson());
}

void PsTab::writeModule()
{
    const auto module = static_cast<ps::Rs485Module>(m_module->currentData().toInt());
    ps::Rs485Values values;
    for (const auto& [key, row] : m_moduleRows) {
        if (!row.send->isChecked()) {
            continue;
        }
        double v = 0.0;
        switch (row.kind) {
        case ps::Rs485FieldKind::Flag: v = static_cast<QCheckBox*>(row.editor)->isChecked() ? 1 : 0; break;
        case ps::Rs485FieldKind::PowerDbm: v = static_cast<QComboBox*>(row.editor)->currentData().toInt(); break;
        case ps::Rs485FieldKind::FrequencyHz:
            v = std::round(static_cast<QDoubleSpinBox*>(row.editor)->value() * 1e6);
            break;
        case ps::Rs485FieldKind::Att31:
        case ps::Rs485FieldKind::Att15:
            v = std::round(static_cast<QDoubleSpinBox*>(row.editor)->value() * 2.0) / 2.0;
            break;
        }
        values[key] = v;
    }
    json payload;
    std::string error;
    if (!ps::rs485SetPayload(module, values, payload, &error)) {
        log(QString::fromStdString(error));
        return;
    }
    send(ps::msg::Rs485Set, payload);
}

void PsTab::requestModuleStatus()
{
    const auto module = static_cast<ps::Rs485Module>(m_module->currentData().toInt());
    send(ps::msg::Rs485Status, ps::rs485StatusPayload(module));
}

void PsTab::showModuleValues(const ps::Rs485Status& st)
{
    if (static_cast<int>(st.module) != m_module->currentData().toInt()) {
        return;
    }
    for (auto& [key, row] : m_moduleRows) {
        const auto it = st.values.find(key);
        row.actual->setText(it == st.values.end() ? QStringLiteral("—") : fieldText(row.kind, it->second));
        row.send->setChecked(false);
    }
    m_moduleStatus->setText(
        QStringLiteral("Temp: %1, Lock: %2")
            .arg(st.temp ? QStringLiteral("%1 °C").arg(*st.temp, 0, 'f', 1) : QStringLiteral("—"))
            .arg(st.lock ? (*st.lock ? QStringLiteral("захват") : QStringLiteral("нет захвата"))
                         : QStringLiteral("—")));
}

void PsTab::showCodes(const std::vector<ps::ChannelCodes>& codes)
{
    for (const auto& c : codes) {
        const int row = c.n - 1;
        const int wantPh = static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->value();
        const int wantAtt = static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->value();
        auto* ph = m_codes->item(row, 3);
        auto* att = m_codes->item(row, 4);
        ph->setText(QString::number(c.ph));
        att->setText(QString::number(c.att));
        // Расхождение с заданным — текстом, а не только цветом (ТЗ т. 5.2).
        if (c.ph != wantPh) {
            ph->setText(QStringLiteral("%1 ≠ %2").arg(c.ph).arg(wantPh));
        }
        if (c.att != wantAtt) {
            att->setText(QStringLiteral("%1 ≠ %2").arg(c.att).arg(wantAtt));
        }
    }
}

void PsTab::setMode(ps::Mode mode, const QString& state)
{
    m_mode = mode;
    m_modeKnown = true;
    QString text = QStringLiteral("режим: %1").arg(QString::fromStdString(ps::modeRu(mode)));
    if (!state.isEmpty()) {
        text += QStringLiteral(", состояние: %1").arg(QString::fromStdString(ps::stateRu(state.toStdString())));
    }
    m_modeLabel->setText(text);
    refreshEnabled();
}

void PsTab::onResponse(const ps::Envelope& r, ps::Status status)
{
    const QString type = qs(r.msg_type);
    if (status != ps::Status::Ok) {
        log(QStringLiteral("%1: %2 (%3)")
                .arg(type, QString::fromStdString(ps::statusRu(status)), qs(ps::toString(status))));
        return;
    }
    std::string error;
    if (r.msg_type == ps::msg::GetState) {
        ps::StateInfo info;
        if (ps::StateInfo::fromJson(r.payload, info, &error)) {
            setMode(info.mode, QString::fromStdString(info.state));
        }
    } else if (r.msg_type == ps::msg::SetMode) {
        ps::Mode mode{};
        const auto it = r.payload.find("mode");
        if (it != r.payload.end() && it->is_string() && ps::parseMode(it->get<std::string>(), mode)) {
            setMode(mode, QString());
        }
        send(ps::msg::GetState);
    } else if (r.msg_type == ps::msg::SetPhaseAtten || r.msg_type == ps::msg::GetPhaseAtten) {
        std::vector<ps::ChannelCodes> codes;
        if (ps::channelsFromJson(r.payload, codes, &error)) {
            showCodes(codes);
        }
    } else if (r.msg_type == ps::msg::SetAdc || r.msg_type == ps::msg::GetAdc) {
        ps::AdcParams adc;
        if (ps::AdcParams::fromJson(r.payload, adc, &error)) {
            m_adcActual->setText(QStringLiteral("установлено: lo %1, bw %2, fs %3, gain %4 дБ")
                                     .arg(hzText(double(adc.lo)), hzText(double(adc.bw)),
                                          hzText(double(adc.fs)))
                                     .arg(adc.gain, 0, 'f', 1));
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
    } else if (r.msg_type == ps::msg::Rs485Set || r.msg_type == ps::msg::Rs485Status) {
        ps::Rs485Status st;
        if (ps::Rs485Status::fromJson(r.payload, st, &error)) {
            showModuleValues(st);
        }
    }
    if (!error.empty()) {
        log(QStringLiteral("%1: ответ не разобран — %2").arg(type, QString::fromStdString(error)));
    }
}

void PsTab::onTelemetry(const ps::Telemetry& t)
{
    m_tlmCounters->setText(QStringLiteral("принято %1, потеряно %2, отброшено %3")
                               .arg(m_link->telemetryPackets())
                               .arg(m_link->telemetryLost())
                               .arg(m_link->telemetryRejected()));
    std::vector<std::pair<QString, QString>> rows;
    rows.emplace_back(QStringLiteral("Режим"), QString::fromStdString(ps::modeRu(t.mode)));
    if (!t.state.empty()) {
        rows.emplace_back(QStringLiteral("Состояние"), QString::fromStdString(ps::stateRu(t.state)));
    }
    rows.emplace_back(QStringLiteral("Время работы"), QStringLiteral("%1 с").arg(t.uptime));
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
    if (!m_modeKnown || t.mode != m_mode) {
        setMode(t.mode, QString::fromStdString(t.state));
    }
}

void PsTab::onFrame(bool outgoing, const QString& channel, const QString& line)
{
    if (channel == QLatin1String("UDP") && !m_logTelemetry->isChecked()) {
        return;
    }
    log(QStringLiteral("%1 %2 %3").arg(outgoing ? QStringLiteral("→") : QStringLiteral("←"), channel, line));
}

void PsTab::setSeriesActive(bool active)
{
    if (active == m_seriesActive) {
        return;
    }
    m_seriesActive = active;
    if (active && m_link->state() != PsLink::State::Disconnected) {
        // PS держит одно соединение: на время серии его занимает контроллер.
        m_reconnectAfterSeries = true;
        m_link->disconnectFromPs();
        log(QStringLiteral("Серия: связь вкладки с PS закрыта, PS управляет оркестратор"));
    } else if (!active && m_reconnectAfterSeries) {
        m_reconnectAfterSeries = false;
        onConnectClicked();
    }
    refreshEnabled();
}

void PsTab::refreshEnabled()
{
    m_connect->setEnabled(!m_seriesActive);
    const bool ready = m_link->isReady() && !m_seriesActive;
    const bool service = ready && m_modeKnown && m_mode == ps::Mode::Service;
    m_toService->setEnabled(ready && (!m_modeKnown || m_mode == ps::Mode::Idle));
    m_toIdle->setEnabled(ready && m_modeKnown && m_mode != ps::Mode::Idle);
    m_getState->setEnabled(ready);
    for (QWidget* w : m_serviceControls) {
        w->setEnabled(service);
    }
}

void PsTab::log(const QString& text)
{
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), text));
}
