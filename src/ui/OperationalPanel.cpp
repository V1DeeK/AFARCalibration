#include "OperationalPanel.h"

#include "PsSession.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

constexpr double kMhz = 1e6;

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QDoubleSpinBox* degBox(QWidget* parent, const char* name, double value)
{
    auto* box = new QDoubleSpinBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    box->setDecimals(1);
    box->setRange(-ps::kAzimuthLimitDeg, ps::kAzimuthLimitDeg);
    box->setSuffix(QStringLiteral("°"));
    box->setValue(value);
    box->setFixedWidth(68);
    return box;
}

/// Выпадающий список значений протокола с русскими подписями.
QComboBox* choiceBox(QWidget* parent, const char* name, std::initializer_list<std::pair<const char*, QString>> items)
{
    auto* box = new QComboBox(parent);
    box->setObjectName(QString::fromLatin1(name));
    for (const auto& [value, title] : items) {
        box->addItem(title, QString::fromLatin1(value));
    }
    return box;
}

std::string choice(const QComboBox* box)
{
    return box->currentData().toString().toStdString();
}

void setChoice(QComboBox* box, const json& payload, const char* key)
{
    const auto it = payload.find(key);
    if (it == payload.end() || !it->is_string() || box->hasFocus()) {
        return;
    }
    const int index = box->findData(QString::fromStdString(it->get<std::string>()));
    if (index >= 0) {
        box->setCurrentIndex(index);
    }
}

void setNumber(QDoubleSpinBox* box, const json& payload, const char* key, double scale = 1.0)
{
    const auto it = payload.find(key);
    if (it != payload.end() && it->is_number() && !box->hasFocus()) {
        box->setValue(it->get<double>() / scale);
    }
}

QTableWidget* resultTable(QWidget* parent, const char* name, const QStringList& headers)
{
    auto* table = new QTableWidget(0, static_cast<int>(headers.size()), parent);
    table->setObjectName(QString::fromLatin1(name));
    table->setHorizontalHeaderLabels(headers);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setMinimumHeight(100);
    // Высоту задаёт окошко, а не «естественный» размер таблицы: верхний ряд «Настроек» невысокий.
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    return table;
}

QString deg(double v)
{
    return QStringLiteral("%1°").arg(v, 0, 'f', 1);
}

}  // namespace

OperationalPanel::OperationalPanel(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("opPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kInnerGap);

    layout->addWidget(buildSettings());
    layout->addWidget(buildActions());

    m_state = ui::hintLabel(QString(), this);
    m_state->setObjectName(QStringLiteral("opState"));
    layout->addWidget(m_state);
    layout->addWidget(buildResults(), 1);

    connect(m_session->link(), &PsLink::responseReceived, this, &OperationalPanel::onResponse);
    connect(m_session->link(), &PsLink::eventReceived, this, &OperationalPanel::onEvent);
    connect(m_session, &PsSession::controlsChanged, this, &OperationalPanel::refreshControls);
    connect(m_session, &PsSession::modeChanged, this, [this] {
        refreshControls();
        if (m_afterModeChange && m_session->modeKnown() && m_session->mode() == ps::Mode::Operational) {
            auto action = std::move(m_afterModeChange);
            m_afterModeChange = nullptr;
            action();
        }
    });
    refreshControls();
}

QWidget* OperationalPanel::buildSettings()
{
    auto* box = new QWidget(this);
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(ui::kInnerGap);
    grid->setVerticalSpacing(ui::kInnerGap);

    const auto button = [this, box](const QString& text, const char* name, const QString& tip,
                                    ui::ButtonRole role = ui::ButtonRole::Secondary) {
        auto* b = new QPushButton(text, box);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolTip(tip);
        ui::setButtonRole(b, role);
        m_buttons.push_back(b);
        return b;
    };
    const auto row = [grid, box](int r, const QString& title, std::initializer_list<QWidget*> editors,
                                 QPushButton* write, QPushButton* read) {
        grid->addWidget(new QLabel(title, box), r, 0);
        auto* line = new QHBoxLayout;
        line->setSpacing(ui::kInnerGap);
        for (QWidget* w : editors) {
            line->addWidget(w);
        }
        line->addStretch(1);
        grid->addLayout(line, r, 1);
        grid->addWidget(write, r, 2);
        grid->addWidget(read, r, 3);
    };

    // Частота (таблица 16): установка сразу запускает калибровку каналов.
    m_freq = new QDoubleSpinBox(box);
    m_freq->setObjectName(QStringLiteral("opFreq"));
    m_freq->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_freq->setDecimals(3);
    m_freq->setRange(double(ps::kSffFrequencyMinHz) / kMhz, double(ps::kSffFrequencyMaxHz) / kMhz);
    m_freq->setSuffix(QStringLiteral(" МГц"));
    m_freq->setValue(5500.0);
    auto* setFreq = button(QStringLiteral("Установить"), "opSetFrequency",
                           QStringLiteral("set_op_frequency: частота и калибровка каналов, ответ — после калибровки"),
                           ui::ButtonRole::Primary);
    auto* getFreq = button(QStringLiteral("Прочитать"), "opGetFrequency", QStringLiteral("get_op_frequency"));
    row(0, QStringLiteral("Частота"), {m_freq}, setFreq, getFreq);

    // Калибровка каналов (таблица 20).
    m_cal = new QLabel(QStringLiteral("калибровка: —"), box);
    m_cal->setObjectName(QStringLiteral("opCal"));
    m_cal->setProperty("role", QStringLiteral("status"));
    auto* startCal = button(QStringLiteral("Калибровать"), "opStartCalibration",
                            QStringLiteral("set_op_start_calibration: ответ — после калибровки"));
    auto* getCal = button(QStringLiteral("Прочитать"), "opGetCalibration", QStringLiteral("get_op_calibration"));
    row(1, QStringLiteral("Калибровка"), {m_cal}, startCal, getCal);

    // Политика захвата и сброса (таблица 17).
    m_acq = choiceBox(box, "opAcq", {{"AUTO", QStringLiteral("авто")}, {"CONFIRM", QStringLiteral("по команде")}});
    m_loss = choiceBox(box, "opLoss", {{"AUTO", QStringLiteral("авто")}, {"CONFIRM", QStringLiteral("по команде")}});
    m_acq->setToolTip(QStringLiteral("Захват (acq): найденный источник захватывается сам (авто) или по кнопке «Захват»"));
    m_loss->setToolTip(QStringLiteral("Сброс (loss): потерянная цель сбрасывается сама (авто) или по кнопке «Сброс цели»"));
    auto* setPolicy = button(QStringLiteral("Записать"), "opSetPolicy", QStringLiteral("set_op_policy"),
                             ui::ButtonRole::Primary);
    auto* getPolicy = button(QStringLiteral("Прочитать"), "opGetPolicy", QStringLiteral("get_op_policy"));
    row(2, QStringLiteral("Политика"), {m_acq, m_loss}, setPolicy, getPolicy);

    // Управление лучом (таблица 18).
    m_rayMode = choiceBox(box, "opRayMode", {{"SCAN", QStringLiteral("сканирование")},
                                             {"FIXED", QStringLiteral("фиксированный")}});
    m_azMin = degBox(box, "opAzMin", -ps::kAzimuthLimitDeg);
    m_azMax = degBox(box, "opAzMax", ps::kAzimuthLimitDeg);
    m_azFix = degBox(box, "opAzFix", 0.0);
    m_azMin->setToolTip(QStringLiteral("az_min — для сканирования"));
    m_azMax->setToolTip(QStringLiteral("az_max — для сканирования"));
    m_azFix->setToolTip(QStringLiteral("az_fix — для фиксированного угла"));
    auto* setRay = button(QStringLiteral("Записать"), "opSetRay", QStringLiteral("set_op_ray"), ui::ButtonRole::Primary);
    auto* getRay = button(QStringLiteral("Прочитать"), "opGetRay", QStringLiteral("get_op_ray"));
    row(3, QStringLiteral("Луч"), {m_rayMode, m_azMin, m_azMax, m_azFix}, setRay, getRay);

    // Источник данных пеленга (таблица 19).
    m_data = choiceBox(box, "opData", {{"AUTO", QStringLiteral("авто")},
                                       {"FPGA", QStringLiteral("цифровой модуль")},
                                       {"VIDEO", QStringLiteral("видеоприёмник")}});
    auto* setData = button(QStringLiteral("Записать"), "opSetData", QStringLiteral("set_op_data"),
                           ui::ButtonRole::Primary);
    auto* getData = button(QStringLiteral("Прочитать"), "opGetData", QStringLiteral("get_op_data"));
    row(4, QStringLiteral("Пеленг по"), {m_data}, setData, getData);
    grid->setColumnStretch(1, 1);

    // Видны только поля выбранного вида луча: окошко узкое.
    const auto rayEnabled = [this] {
        const bool scan = choice(m_rayMode) == "SCAN";
        m_azMin->setVisible(scan);
        m_azMax->setVisible(scan);
        m_azFix->setVisible(!scan);
    };
    rayEnabled();
    connect(m_rayMode, &QComboBox::currentIndexChanged, this, rayEnabled);

    connect(setFreq, &QPushButton::clicked, this, [this] {
        send(ps::msg::SetOpFrequency, json{{"freq", std::llround(m_freq->value() * kMhz)}});
    });
    connect(getFreq, &QPushButton::clicked, this, [this] { send(ps::msg::GetOpFrequency); });
    connect(startCal, &QPushButton::clicked, this, [this] { send(ps::msg::SetOpStartCalibration); });
    connect(getCal, &QPushButton::clicked, this, [this] { send(ps::msg::GetOpCalibration); });
    connect(setPolicy, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetOpPolicy, json{{"acq", choice(m_acq)}, {"loss", choice(m_loss)}}); });
    connect(getPolicy, &QPushButton::clicked, this, [this] { send(ps::msg::GetOpPolicy); });
    connect(setRay, &QPushButton::clicked, this, [this] {
        if (choice(m_rayMode) == "SCAN" && m_azMin->value() > m_azMax->value()) {
            m_state->setText(QStringLiteral("Не отправлено: начало сектора больше конца"));
            return;
        }
        send(ps::msg::SetOpRay, json{{"mode", choice(m_rayMode)}, {"az_min", m_azMin->value()},
                                     {"az_max", m_azMax->value()}, {"az_fix", m_azFix->value()}});
    });
    connect(getRay, &QPushButton::clicked, this, [this] { send(ps::msg::GetOpRay); });
    connect(setData, &QPushButton::clicked, this, [this] { send(ps::msg::SetOpData, json{{"data", choice(m_data)}}); });
    connect(getData, &QPushButton::clicked, this, [this] { send(ps::msg::GetOpData); });
    return box;
}

QWidget* OperationalPanel::buildActions()
{
    auto* box = new QWidget(this);
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(ui::kInnerGap);
    grid->setVerticalSpacing(ui::kInnerGap);

    const auto button = [this, box](const QString& text, const char* name, const QString& tip, ui::ButtonRole role) {
        auto* b = new QPushButton(text, box);
        b->setObjectName(QString::fromLatin1(name));
        b->setToolTip(tip);
        ui::setButtonRole(b, role);
        m_buttons.push_back(b);
        return b;
    };
    auto* survey = button(QStringLiteral("Поиск"), "opStartSurvey",
                          QStringLiteral("set_op_start_survey: из ожидания (OP_IDLE) в поиск"), ui::ButtonRole::Success);
    auto* loss = button(QStringLiteral("Сброс цели"), "opStartLoss",
                        QStringLiteral("set_op_start_loss: из сопровождения в ограниченный поиск"),
                        ui::ButtonRole::Secondary);
    auto* stop = button(QStringLiteral("Стоп"), "opStop",
                        QStringLiteral("set_op_stop: завершить поиск, захват, сопровождение — в ожидание"),
                        ui::ButtonRole::Danger);
    m_acquireAz = degBox(box, "opAcquireAz", 0.0);
    auto* acquire = button(QStringLiteral("Захват"), "opStartAcquire",
                           QStringLiteral("set_op_start_acquire: захват источника на азимуте"),
                           ui::ButtonRole::Primary);
    m_holdAz = degBox(box, "opHoldAz", 0.0);
    auto* hold = button(QStringLiteral("Удерживать"), "opStartHold",
                        QStringLiteral("set_op_start_hold: удерживать азимут; при политике луча «фиксированный» "
                                       "PS команду не выполняет"),
                        ui::ButtonRole::Secondary);
    auto* unhold = button(QStringLiteral("Отпустить"), "opStopHold", QStringLiteral("set_op_stop_hold"),
                          ui::ButtonRole::Secondary);
    m_hold = new QLabel(QStringLiteral("удержание: —"), box);
    m_hold->setObjectName(QStringLiteral("opHold"));
    m_hold->setProperty("role", QStringLiteral("hint"));

    grid->addWidget(new QLabel(QStringLiteral("Работа"), box), 0, 0);
    auto* work = new QHBoxLayout;
    work->setSpacing(ui::kInnerGap);
    work->addWidget(survey);
    work->addWidget(loss);
    work->addWidget(stop);
    work->addStretch(1);
    grid->addLayout(work, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("Захват на"), box), 1, 0);
    auto* acq = new QHBoxLayout;
    acq->setSpacing(ui::kInnerGap);
    acq->addWidget(m_acquireAz);
    acq->addWidget(acquire);
    acq->addStretch(1);
    grid->addLayout(acq, 1, 1);
    grid->addWidget(new QLabel(QStringLiteral("Удержание"), box), 2, 0);
    auto* hl = new QHBoxLayout;
    hl->setSpacing(ui::kInnerGap);
    hl->addWidget(m_holdAz);
    hl->addWidget(hold);
    hl->addWidget(unhold);
    hl->addWidget(m_hold, 1);
    grid->addLayout(hl, 2, 1);
    grid->setColumnStretch(1, 1);

    connect(survey, &QPushButton::clicked, this, [this] { send(ps::msg::SetOpStartSurvey); });
    connect(loss, &QPushButton::clicked, this, [this] { send(ps::msg::SetOpStartLoss); });
    connect(stop, &QPushButton::clicked, this, [this] { send(ps::msg::SetOpStop); });
    connect(acquire, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetOpStartAcquire, json{{"az", m_acquireAz->value()}}); });
    connect(hold, &QPushButton::clicked, this,
            [this] { send(ps::msg::SetOpStartHold, json{{"az", m_holdAz->value()}}); });
    connect(unhold, &QPushButton::clicked, this, [this] { send(ps::msg::SetOpStopHold); });
    return box;
}

QWidget* OperationalPanel::buildResults()
{
    // События — две вкладки, чтобы окошко в верхнем ряду «Настроек» оставалось невысоким.
    auto* box = new QTabWidget(this);
    box->setObjectName(QStringLiteral("opResults"));
    box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    box->setMinimumHeight(150);
    m_survey = resultTable(box, "opSurvey",
                           {QStringLiteral("Азимут"), QStringLiteral("Ширина, МГц"), QStringLiteral("С/Ш, дБ"),
                            QStringLiteral("Класс"), QStringLiteral("Достоверность"), QStringLiteral("Когерентных, %"),
                            QStringLiteral("Для пеленга, %")});
    m_survey->setToolTip(QStringLiteral("send_op_survey_result; двойной щелчок по строке подставляет азимут захвата"));
    box->addTab(m_survey, QStringLiteral("Результаты поиска"));
    m_track = resultTable(box, "opTrack",
                          {QStringLiteral("Время"), QStringLiteral("Состояние"), QStringLiteral("Луч"),
                           QStringLiteral("Пеленг"), QStringLiteral("Прогноз"), QStringLiteral("Ошибка"),
                           QStringLiteral("Скорость, °/с"), QStringLiteral("С/Ш, дБ"), QStringLiteral("Класс"),
                           QStringLiteral("Источник"), QStringLiteral("Метод")});
    m_track->setToolTip(QStringLiteral("send_op_track_result, новые строки сверху"));
    box->addTab(m_track, QStringLiteral("Захват и сопровождение"));

    connect(m_survey, &QTableWidget::cellDoubleClicked, this, [this](int r, int) {
        if (auto* item = m_survey->item(r, 0)) {
            m_acquireAz->setValue(item->data(Qt::UserRole).toDouble());
        }
    });
    return box;
}

void OperationalPanel::inOpMode(std::function<void()> action)
{
    if (!m_session->manualAllowed()) {
        return;
    }
    if (m_session->modeKnown() && m_session->mode() == ps::Mode::Operational) {
        action();
        return;
    }
    m_afterModeChange = std::move(action);
    m_state->setText(QStringLiteral("Перевожу изделие в оперативный режим…"));
    if (m_session->send(ps::msg::SetMode, json{{"mode", "OPERATIONAL"}}) < 0) {
        m_afterModeChange = nullptr;
        m_state->setText(m_session->lastError());
    }
}

void OperationalPanel::send(std::string_view msgType, const json& payload)
{
    inOpMode([this, type = std::string(msgType), payload] {
        if (m_session->send(type, payload) >= 0) {
            m_state->setText(QStringLiteral("%1 отправлена, ждём ответа PS").arg(QString::fromStdString(type)));
            ui::setTone(m_state, ui::Tone::Neutral);
        }
    });
}

void OperationalPanel::onResponse(const ps::Envelope& r, ps::Status status)
{
    const std::string& t = r.msg_type;
    if (t.rfind("set_op_", 0) != 0 && t.rfind("get_op_", 0) != 0) {
        return;
    }
    if (status != ps::Status::Ok) {
        m_state->setText(QStringLiteral("%1: PS отказал — %2 (%3)")
                             .arg(qs(t), QString::fromStdString(ps::statusRu(status)), qs(ps::toString(status))));
        ui::setTone(m_state, ui::Tone::Error);
        return;
    }
    m_state->setText(QStringLiteral("%1: OK").arg(qs(t)));
    ui::setTone(m_state, ui::Tone::Ok);
    const json& p = r.payload;
    setNumber(m_freq, p, "freq", kMhz);
    setChoice(m_acq, p, "acq");
    setChoice(m_loss, p, "loss");
    setChoice(m_rayMode, p, "mode");
    setNumber(m_azMin, p, "az_min");
    setNumber(m_azMax, p, "az_max");
    setNumber(m_azFix, p, "az_fix");
    setChoice(m_data, p, "data");
    if (const auto cal = p.find("cal"); cal != p.end() && cal->is_string()) {
        const bool ok = cal->get<std::string>() == "OK";
        m_cal->setText(ok ? QStringLiteral("калибровка выполнена") : QStringLiteral("калибровка не выполнена"));
        ui::setTone(m_cal, ok ? ui::Tone::Ok : ui::Tone::Warn);
    }
    if (t == ps::msg::SetOpFrequency) {
        // Установка частоты включает калибровку каналов (п. 3.5.1.1).
        m_cal->setText(QStringLiteral("калибровка выполнена при установке частоты"));
        ui::setTone(m_cal, ui::Tone::Ok);
    } else if (t == ps::msg::SetOpStartHold) {
        const auto az = p.find("az");
        m_hold->setText(az != p.end() && az->is_number() ? QStringLiteral("удержание: %1").arg(deg(az->get<double>()))
                                                         : QStringLiteral("удержание: включено"));
    } else if (t == ps::msg::SetOpStopHold) {
        m_hold->setText(QStringLiteral("удержание: выключено"));
    }
    // Команды, меняющие вложенный КА, — сразу узнаём новое состояние.
    if (t == ps::msg::SetOpFrequency || t == ps::msg::SetOpStartCalibration || t == ps::msg::SetOpStartSurvey ||
        t == ps::msg::SetOpStartAcquire || t == ps::msg::SetOpStartLoss || t == ps::msg::SetOpStop) {
        m_session->send(ps::msg::GetState);
    }
}

void OperationalPanel::onEvent(const ps::Envelope& event)
{
    std::string error;
    if (event.msg_type == ps::msg::SendOpSurveyResult) {
        ps::OpSurveyResult r;
        if (!ps::OpSurveyResult::fromJson(event.payload, r, &error)) {
            m_state->setText(QString::fromStdString(error));
            ui::setTone(m_state, ui::Tone::Error);
            return;
        }
        m_survey->setRowCount(static_cast<int>(r.tgt.size()));
        for (int row = 0; row < static_cast<int>(r.tgt.size()); ++row) {
            const ps::OpTarget& t = r.tgt[static_cast<std::size_t>(row)];
            const QStringList cells{deg(t.az),
                                    QString::number(t.w, 'f', 1),
                                    QString::number(t.snr, 'f', 1),
                                    QString::fromStdString(ps::opClassRu(t.cls)),
                                    QString::number(t.conf, 'f', 2),
                                    QString::number(t.coh, 'f', 0),
                                    QString::number(t.df, 'f', 0)};
            for (int c = 0; c < cells.size(); ++c) {
                auto* item = new QTableWidgetItem(cells.at(c));
                if (c == 0) {
                    item->setData(Qt::UserRole, t.az);
                }
                m_survey->setItem(row, c, item);
            }
        }
        m_state->setText(r.tgt.empty() ? QStringLiteral("Поиск: источников не обнаружено")
                                       : QStringLiteral("Поиск: источников %1").arg(r.tgt.size()));
        ui::setTone(m_state, ui::Tone::Info);
    } else if (event.msg_type == ps::msg::SendOpTrackResult) {
        ps::OpTrackResult r;
        if (!ps::OpTrackResult::fromJson(event.payload, r, &error)) {
            m_state->setText(QString::fromStdString(error));
            ui::setTone(m_state, ui::Tone::Error);
            return;
        }
        m_track->insertRow(0);
        const QStringList cells{QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                                QString::fromStdString(ps::opTrackStateRu(r.st)),
                                deg(r.az_ray),
                                deg(r.az),
                                deg(r.az_pred),
                                deg(r.az_err),
                                QString::number(r.az_rate, 'f', 2),
                                QString::number(r.target.snr, 'f', 1),
                                QString::fromStdString(ps::opClassRu(r.target.cls)),
                                QString::fromStdString(r.src),
                                QString::fromStdString(r.meth)};
        for (int c = 0; c < cells.size(); ++c) {
            m_track->setItem(0, c, new QTableWidgetItem(cells.at(c)));
        }
        if (m_track->rowCount() > kTrackRows) {
            m_track->setRowCount(kTrackRows);
        }
    }
}

void OperationalPanel::refreshControls()
{
    const bool allowed = m_session->manualAllowed();
    for (QPushButton* b : m_buttons) {
        b->setEnabled(allowed);
    }
}
