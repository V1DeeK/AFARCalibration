#include "ParametersTab.h"

#include "PsExchangeView.h"
#include "PsSession.h"

#include "PsModulesPage.h"

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

ParametersTab::ParametersTab(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    auto* top = new QHBoxLayout;
    top->addWidget(buildModeBox(), 1);
    top->addWidget(buildAdcBox(), 1);
    top->addWidget(buildBeamBox(), 1);
    root->addLayout(top);

    auto* modulesBox = new QGroupBox(QStringLiteral("Модули"), this);
    auto* modulesLayout = new QVBoxLayout(modulesBox);
    m_modulesPage = new PsModulesPage(m_session->link(), modulesBox);
    m_modulesPage->tabs()->addTab(buildCodesPage(), QStringLiteral("Коды решётки"));
    modulesLayout->addWidget(m_modulesPage);
    root->addWidget(modulesBox, 3);
    root->addWidget(buildBottomTabs(), 2);

    connect(m_session->link(), &PsLink::responseReceived, this, &ParametersTab::onResponse);
    connect(m_session, &PsSession::telemetryChanged, this, &ParametersTab::onTelemetry);
    connect(m_session, &PsSession::modeChanged, this, &ParametersTab::refreshMode);
    connect(m_session, &PsSession::controlsChanged, this, &ParametersTab::refreshEnabled);
    connect(m_session, &PsSession::message, this, &ParametersTab::log);
    refreshMode();
}

ParametersTab::~ParametersTab() = default;

QWidget* ParametersTab::buildBottomTabs()
{
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("parametersBottomTabs"));
    auto* exchangePage = new QWidget(tabs);
    auto* exchangeLayout = new QVBoxLayout(exchangePage);
    m_exchange = new PsExchangeView(m_session, exchangePage);
    m_exchange->table()->setObjectName(QStringLiteral("psExchange"));
    m_log = new QPlainTextEdit(exchangePage);
    m_log->setObjectName(QStringLiteral("psLog"));
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(kLogLines);
    m_log->setMaximumHeight(60);
    m_log->setPlaceholderText(QStringLiteral("ошибки связи, отказы PS и сообщения программы"));
    exchangeLayout->addWidget(m_exchange, 1);
    exchangeLayout->addWidget(m_log);
    tabs->addTab(exchangePage, QStringLiteral("Обмен с PS"));

    auto* tlmPage = new QWidget(tabs);
    auto* tlmLayout = new QVBoxLayout(tlmPage);
    m_tlmCounters = new QLabel(QStringLiteral("датаграмм нет"), tlmPage);
    m_tlmCounters->setObjectName(QStringLiteral("psTlmCounters"));
    m_tlmTable = new QTableWidget(0, 2, tlmPage);
    m_tlmTable->setObjectName(QStringLiteral("psTlmTable"));
    m_tlmTable->setHorizontalHeaderLabels({QStringLiteral("Параметр"), QStringLiteral("Значение")});
    m_tlmTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tlmTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tlmTable->verticalHeader()->setVisible(false);
    m_tlmTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tlmLayout->addWidget(m_tlmCounters);
    tlmLayout->addWidget(m_tlmTable, 1);
    tabs->addTab(tlmPage, QStringLiteral("Телеметрия"));
    return tabs;
}

QTableWidget* ParametersTab::exchangeTable() const
{
    return m_exchange->table();
}

QWidget* ParametersTab::buildModeBox()
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

QWidget* ParametersTab::buildCodesPage()
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
    connect(m_writeCodes, &QPushButton::clicked, this, &ParametersTab::writeCodes);
    connect(m_readCodes, &QPushButton::clicked, this, [this] { send(ps::msg::GetPhaseAtten); });
    m_serviceControls.insert(m_serviceControls.end(), {m_writeCodes, m_readCodes});
    return page;
}

QWidget* ParametersTab::buildAdcBox()
{
    auto* page = new QGroupBox(QStringLiteral("АЦП"), this);
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

    connect(apply, &QPushButton::clicked, this, &ParametersTab::applyAdc);
    connect(read, &QPushButton::clicked, this, [this] { send(ps::msg::GetAdc); });
    m_serviceControls.insert(m_serviceControls.end(), {apply, read});
    return page;
}

QWidget* ParametersTab::buildBeamBox()
{
    auto* page = new QGroupBox(QStringLiteral("Луч"), this);
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

void ParametersTab::send(std::string_view msgType, const json& payload)
{
    m_session->send(msgType, payload);
}

void ParametersTab::writeCodes()
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

bool ParametersTab::saveCodes(const QString& path, QString* error) const
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

bool ParametersTab::loadCodes(const QString& path, QString* error)
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

void ParametersTab::applyAdc()
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

void ParametersTab::showCodes(const std::vector<ps::ChannelCodes>& codes)
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

void ParametersTab::onResponse(const ps::Envelope& r, ps::Status status)
{
    const QString type = qs(r.msg_type);
    if (status != ps::Status::Ok) {
        return;  // отказ PS уже в журнале сообщений сессии
    }
    std::string error;
    if (r.msg_type == ps::msg::SetPhaseAtten || r.msg_type == ps::msg::GetPhaseAtten) {
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

void ParametersTab::onTelemetry()
{
    const PsLink* link = m_session->link();
    m_tlmCounters->setText(QStringLiteral("принято %1, потеряно %2, отброшено %3")
                               .arg(link->telemetryPackets())
                               .arg(link->telemetryLost())
                               .arg(link->telemetryRejected()));
    const auto& tlm = m_session->telemetry();
    if (!tlm) {
        return;
    }
    const ps::Telemetry& t = *tlm;
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
}

void ParametersTab::refreshMode()
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
    refreshEnabled();
}

void ParametersTab::refreshEnabled()
{
    const bool ready = m_session->manualAllowed();
    const bool known = m_session->modeKnown();
    const ps::Mode mode = m_session->mode();
    const bool service = ready && known && mode == ps::Mode::Service;
    m_modulesPage->setAllowed(service);
    m_toService->setEnabled(ready && (!known || mode == ps::Mode::Idle));
    m_toIdle->setEnabled(ready && known && mode != ps::Mode::Idle);
    m_getState->setEnabled(ready);
    for (QWidget* w : m_serviceControls) {
        w->setEnabled(service);
    }
}

void ParametersTab::log(const QString& text)
{
    m_log->appendPlainText(QStringLiteral("%1  %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")), text));
}
