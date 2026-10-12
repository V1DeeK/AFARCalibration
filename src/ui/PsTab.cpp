#include "PsTab.h"

#include "PsPacketPage.h"
#include "PsServiceCheckPage.h"
#include "PsTrafficView.h"

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
    m_pages->addTab(buildRawPage(), QStringLiteral("RS-485 HEX"));
    m_packetPage = new PsPacketPage(m_link, this);
    m_pages->addTab(m_packetPage, QStringLiteral("Пакет"));
    m_checkPage = new PsServiceCheckPage(m_link, this);
    m_pages->addTab(m_checkPage, QStringLiteral("Приёмка"));
    middle->addWidget(m_pages, 3);
    middle->addWidget(buildTelemetryBox(), 2);
    root->addLayout(middle, 1);

    auto* bottom = new QTabWidget(this);
    m_traffic = new PsTrafficView(m_link, bottom);
    m_traffic->setObjectName(QStringLiteral("psTraffic"));
    bottom->addTab(m_traffic, QStringLiteral("Обмен TCP / UDP"));
    m_log = new QPlainTextEdit(bottom);
    m_log->setObjectName(QStringLiteral("psLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(kLogLines);
    bottom->addTab(m_log, QStringLiteral("События"));
    root->addWidget(bottom, 1);

    connect(m_link, &PsLink::stateChanged, this, &PsTab::onLinkStateChanged);
    connect(m_link, &PsLink::responseReceived, this, &PsTab::onResponse);
    connect(m_link, &PsLink::telemetryReceived, this, &PsTab::onTelemetry);
    connect(m_link, &PsLink::errorOccurred, this,
            [this](const QString& message) { log(QStringLiteral("Ошибка: %1").arg(message)); });
    connect(m_link, &PsLink::commandTimedOut, this, [this](const QString& type, int count) {
        log(QStringLiteral("Нет ответа на %1 #%2").arg(type).arg(count));
    });

    connect(m_checkPage, &PsServiceCheckPage::runningChanged, this, [this](bool) { refreshEnabled(); });

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
    m_port->setObjectName(QStringLiteral("psPort"));
    m_port->setRange(1, 65535);
    m_port->setValue(ps::kDefaultControlPort);
    m_tlmPort = new QSpinBox(box);
    m_tlmPort->setObjectName(QStringLiteral("psTlmPort"));
    m_tlmPort->setRange(1, 65535);
    m_tlmPort->setValue(ps::kDefaultTelemetryPort);
    m_keepAlive = new QSpinBox(box);
    m_keepAlive->setObjectName(QStringLiteral("psKeepAlive"));
    m_keepAlive->setRange(1, 300);
    m_keepAlive->setValue(ps::kDefaultKeepAliveSec);
    m_keepAlive->setSuffix(QStringLiteral(" с"));
    m_keepAlive->setToolTip(QStringLiteral("keep_alive уходит, если в канале не было обмена столько секунд. "
                                           "Нет ответа за 2 с — связь потеряна, переподключение с новым hello."));
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
    grid->addWidget(new QLabel(QStringLiteral("keep_alive при простое"), box), 2, 0);
    grid->addWidget(m_keepAlive, 2, 1);
    grid->addWidget(m_connect, 2, 3);
    grid->addWidget(m_linkState, 3, 0, 1, 4);

    connect(m_connect, &QPushButton::clicked, this, &PsTab::onConnectClicked);
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
    auto* files = new QHBoxLayout;
    auto* saveBtn = new QPushButton(QStringLiteral("Сохранить набор…"), page);
    auto* loadBtn = new QPushButton(QStringLiteral("Загрузить набор…"), page);
    files->addWidget(saveBtn);
    files->addWidget(loadBtn);
    files->addStretch(1);
    layout->addWidget(m_codes, 1);
    layout->addLayout(fill);
    layout->addLayout(files);

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

void PsTab::onConnectClicked()
{
    if (m_link->state() != PsLink::State::Disconnected) {
        m_link->disconnectFromPs();
        return;
    }
    const QString host = m_host->text().trimmed();
    const auto port = static_cast<quint16>(m_port->value());
    if (host.isEmpty()) {
        log(QStringLiteral("Укажите адрес PS"));
        return;
    }
    PsLink::Timeouts t = m_link->timeouts();
    t.keepAliveIdleMs = m_keepAlive->value() * 1000;
    m_link->setTimeouts(t);
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
    const bool idle = state == PsLink::State::Disconnected;
    m_host->setEnabled(idle);
    m_port->setEnabled(idle);
    m_tlmPort->setEnabled(idle);
    m_keepAlive->setEnabled(idle);
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

bool PsTab::saveCodes(const QString& path, QString* error) const
{
    std::vector<ps::ChannelCodes> codes;
    for (int row = 0; row < ps::kChannels; ++row) {
        codes.push_back({row + 1, static_cast<QSpinBox*>(m_codes->cellWidget(row, 1))->value(),
                         static_cast<QSpinBox*>(m_codes->cellWidget(row, 2))->value()});
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

bool PsTab::loadCodes(const QString& path, QString* error)
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
    }
    log(QStringLiteral("Набор кодов загружен: %1").arg(path));
    return true;
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

void PsTab::setSeriesActive(bool active)
{
    if (active == m_seriesActive) {
        return;
    }
    m_seriesActive = active;
    // Связь с PS серия не трогает: подключение анализатора и ход серии на неё
    // не влияют. Блокируются только ручные команды, пока идёт измерение.
    log(active ? QStringLiteral("Идёт серия: ручные команды PS заблокированы, связь сохранена")
               : QStringLiteral("Серия остановлена: ручные команды PS снова доступны"));
    refreshEnabled();
}

void PsTab::refreshEnabled()
{
    // Во время приёмки команды шлёт только она: ручные мешали бы её шагам.
    const bool checking = m_checkPage->isRunning();
    const bool ready = m_link->isReady() && !m_seriesActive && !checking;
    const bool service = ready && m_modeKnown && m_mode == ps::Mode::Service;
    m_checkPage->setAllowed(m_link->isReady() && !m_seriesActive);
    m_packetPage->setAllowed(ready);
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
