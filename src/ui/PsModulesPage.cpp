#include "PsModulesPage.h"

#include "PsLink.h"
#include "UiStyle.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <cmath>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

constexpr std::array<int, 4> kPowers{-4, -1, 2, 5};

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QString prefix(PsModulesPage::Module m)
{
    switch (m) {
    case PsModulesPage::Lo1: return QStringLiteral("psLo1");
    case PsModulesPage::Lo2: return QStringLiteral("psLo2");
    case PsModulesPage::Video: return QStringLiteral("psVideo");
    case PsModulesPage::Mixer:
    case PsModulesPage::ModuleCount: break;
    }
    return QStringLiteral("psMixer");
}

/// Значение для подписи «Прочитано»: частота — в МГц.
QString valueText(const ps::ModuleField& f, double v)
{
    if (f.kind == ps::ModuleFieldKind::FrequencyHz) {
        return QString::number(v / 1e6, 'f', 6) + QStringLiteral(" МГц");
    }
    return QString::number(v);
}

QString stateText(ps::Status status, const json& payload)
{
    QString s = QStringLiteral("Ответ: %1").arg(qs(ps::toString(status)));
    if (status != ps::Status::Ok) {
        return s;
    }
    if (const auto t = payload.find("temp"); t != payload.end() && t->is_number()) {
        s += QStringLiteral(", t = %1 °C").arg(t->get<double>());
    }
    if (const auto l = payload.find("lock"); l != payload.end() && l->is_number()) {
        s += l->get<double>() != 0.0 ? QStringLiteral(", захват частоты есть")
                                     : QStringLiteral(", захвата частоты нет");
    }
    return s;
}

}  // namespace

PsModulesPage::PsModulesPage(PsLink* link, QWidget* parent)
    : QWidget(parent)
    , m_link(link)
{
    auto* layout = new QVBoxLayout(this);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("psModulesTabs"));
    m_tabs->addTab(buildModule(Lo1, ps::loFields()), qs(ps::loNameRu(ps::kLoMixer)));
    m_tabs->addTab(buildModule(Lo2, ps::loFields()), qs(ps::loNameRu(ps::kLoCalibration)));
    m_tabs->addTab(buildMixer(), QStringLiteral("Смеситель"));
    m_tabs->addTab(buildModule(Video, ps::videoFields()), QStringLiteral("Видеоприёмник"));
    // Два гетеродина — одинаковые по командам, но разные устройства на линии RS-485.
    m_tabs->setTabToolTip(0, QStringLiteral("Адрес RS-485 0x01, set_lo/get_lo с addr = 1"));
    m_tabs->setTabToolTip(1, QStringLiteral("Адрес RS-485 0x02, set_lo/get_lo с addr = 2; "
                                            "команды те же, что у гетеродина смесителя"));
    m_tabs->setTabToolTip(2, QStringLiteral("Адрес RS-485 0x03, set_mixer/get_mixer"));
    m_tabs->setTabToolTip(3, QStringLiteral("Адрес RS-485 0x04, set_video/get_video"));
    layout->addWidget(m_tabs, 1);
    m_result = new QLabel(QStringLiteral("В «Записать» уходят только поля с отметкой «Передать», "
                                         "остальные PS не меняет. Кадры RS-485 собирает PS."),
                          this);
    m_result->setObjectName(QStringLiteral("psModulesResult"));
    m_result->setWordWrap(true);
    layout->addWidget(m_result);

    connect(m_link, &PsLink::responseReceived, this, &PsModulesPage::onResponse);
    connect(m_link, &PsLink::commandTimedOut, this, [this](const QString&, int count) {
        if (const auto it = m_pending.find(count); it != m_pending.end()) {
            m_ui[it->second].state->setText(QStringLiteral("PS не ответил"));
            m_pending.erase(it);
        }
    });
    connect(m_link, &PsLink::stateChanged, this, [this](PsLink::State state) {
        if (state != PsLink::State::Ready) {
            m_pending.clear();
        }
    });
    setAllowed(false);
}

QWidget* PsModulesPage::makeEditor(const ps::ModuleField& f, const QString& name, QCheckBox* send)
{
    const auto mark = [this, send] {
        if (!m_filling) {
            send->setChecked(true);
        }
    };
    QWidget* editor = nullptr;
    switch (f.kind) {
    case ps::ModuleFieldKind::Flag: {
        auto* box = new QCheckBox(QStringLiteral("1 — да"), this);
        connect(box, &QCheckBox::toggled, this, mark);
        editor = box;
        break;
    }
    case ps::ModuleFieldKind::PowerDbm: {
        auto* box = new QComboBox(this);
        for (int p : kPowers) {
            box->addItem(QString::number(p).replace(QLatin1Char('-'), QChar(0x2212)), p);
        }
        connect(box, &QComboBox::currentIndexChanged, this, mark);
        editor = box;
        break;
    }
    case ps::ModuleFieldKind::FrequencyHz: {
        auto* box = new QDoubleSpinBox(this);
        box->setDecimals(6);
        box->setRange(0.0, 7000.0);
        box->setSuffix(QStringLiteral(" МГц"));
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        connect(box, &QDoubleSpinBox::valueChanged, this, mark);
        editor = box;
        break;
    }
    case ps::ModuleFieldKind::Att31:
    case ps::ModuleFieldKind::Att15: {
        auto* box = new QDoubleSpinBox(this);
        box->setDecimals(1);
        box->setSingleStep(0.5);
        box->setRange(0.0, f.kind == ps::ModuleFieldKind::Att31 ? 31.5 : 15.5);
        box->setSuffix(QStringLiteral(" дБ"));
        box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        connect(box, &QDoubleSpinBox::valueChanged, this, mark);
        editor = box;
        break;
    }
    }
    editor->setObjectName(name);
    editor->setToolTip(QStringLiteral("%1: %2").arg(qs(f.key), qs(ps::moduleRangeText(f.kind))));
    return editor;
}

double PsModulesPage::editorValue(const ps::ModuleField& f, QWidget* editor) const
{
    switch (f.kind) {
    case ps::ModuleFieldKind::Flag: return static_cast<QCheckBox*>(editor)->isChecked() ? 1.0 : 0.0;
    case ps::ModuleFieldKind::PowerDbm: return static_cast<QComboBox*>(editor)->currentData().toDouble();
    case ps::ModuleFieldKind::FrequencyHz:
        return std::round(static_cast<QDoubleSpinBox*>(editor)->value() * 1e6);
    case ps::ModuleFieldKind::Att31:
    case ps::ModuleFieldKind::Att15: return static_cast<QDoubleSpinBox*>(editor)->value();
    }
    return 0.0;
}

void PsModulesPage::setEditorValue(const ps::ModuleField& f, QWidget* editor, double v)
{
    switch (f.kind) {
    case ps::ModuleFieldKind::Flag: static_cast<QCheckBox*>(editor)->setChecked(v != 0.0); break;
    case ps::ModuleFieldKind::PowerDbm: {
        auto* box = static_cast<QComboBox*>(editor);
        const int i = box->findData(static_cast<int>(v));
        if (i >= 0) {
            box->setCurrentIndex(i);
        }
        break;
    }
    case ps::ModuleFieldKind::FrequencyHz: static_cast<QDoubleSpinBox*>(editor)->setValue(v / 1e6); break;
    case ps::ModuleFieldKind::Att31:
    case ps::ModuleFieldKind::Att15: static_cast<QDoubleSpinBox*>(editor)->setValue(v); break;
    }
}

QWidget* PsModulesPage::buildModule(Module m, const std::vector<ps::ModuleField>& fields)
{
    auto* page = new QWidget(this);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(ui::kInnerGap);
    grid->setVerticalSpacing(ui::kInnerGap);
    auto& mod = m_ui[m];
    // Широкое окошко: поля идут двумя колонками, каждая — «Передать | Поле | Значение | Прочитано».
    const int columns = fields.size() > 6 ? 2 : 1;
    const int perColumn = (static_cast<int>(fields.size()) + columns - 1) / columns;
    constexpr int kColumnWidth = 5;  // send, поле, значение, прочитано, зазор
    for (int c = 0; c < columns; ++c) {
        const int base = c * kColumnWidth;
        grid->addWidget(ui::captionLabel(QStringLiteral("Передать"), page), 0, base);
        grid->addWidget(ui::captionLabel(QStringLiteral("Поле"), page), 0, base + 1);
        grid->addWidget(ui::captionLabel(QStringLiteral("Значение"), page), 0, base + 2);
        grid->addWidget(ui::captionLabel(QStringLiteral("Прочитано из модуля"), page), 0, base + 3);
        grid->setColumnStretch(base + 1, 3);
        grid->setColumnStretch(base + 3, 2);
        grid->setColumnMinimumWidth(base + 2, 150);
        if (c + 1 < columns) {
            grid->setColumnMinimumWidth(base + 4, ui::kGap * 2);
        }
    }
    int index = 0;
    for (const auto& f : fields) {
        const int column = index / perColumn;
        const int row = index % perColumn + 1;
        const int base = column * kColumnWidth;
        const QString name = prefix(m) + QLatin1Char('_') + qs(f.key);
        FieldRow r{f};
        r.send = new QCheckBox(page);
        r.send->setObjectName(name + QStringLiteral("_send"));
        r.editor = makeEditor(f, name, r.send);
        r.actual = new QLabel(QStringLiteral("—"), page);
        r.actual->setProperty("role", QStringLiteral("hint"));
        grid->addWidget(r.send, row, base, Qt::AlignHCenter);
        grid->addWidget(new QLabel(QStringLiteral("%1 (%2)").arg(qs(f.titleRu), qs(f.key)), page), row, base + 1);
        grid->addWidget(r.editor, row, base + 2);
        grid->addWidget(r.actual, row, base + 3);
        mod.rows.push_back(r);
        ++index;
    }

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(ui::kInnerGap);
    mod.write = new QPushButton(QStringLiteral("Записать"), page);
    mod.write->setObjectName(prefix(m) + QStringLiteral("Write"));
    ui::setButtonRole(mod.write, ui::ButtonRole::Primary);
    mod.read = new QPushButton(QStringLiteral("Прочитать"), page);
    mod.read->setObjectName(prefix(m) + QStringLiteral("Read"));
    ui::setButtonRole(mod.read, ui::ButtonRole::Secondary);
    mod.state = new QLabel(QStringLiteral("Ответ: —"), page);
    mod.state->setObjectName(prefix(m) + QStringLiteral("State"));
    mod.state->setProperty("role", QStringLiteral("hint"));
    buttons->addWidget(mod.write);
    buttons->addWidget(mod.read);
    buttons->addWidget(mod.state, 1);
    connect(mod.write, &QPushButton::clicked, this, [this, m] { write(m); });
    connect(mod.read, &QPushButton::clicked, this, [this, m] { read(m); });

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kGap);
    layout->addLayout(grid);
    layout->addStretch(1);
    layout->addLayout(buttons);
    return page;
}

QWidget* PsModulesPage::buildMixer()
{
    auto* page = new QWidget(this);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(ui::kInnerGap);
    grid->setVerticalSpacing(ui::kInnerGap);
    grid->addWidget(ui::captionLabel(QStringLiteral("Передать"), page), 0, 0);
    const auto& fields = ps::mixerChannelFields();
    for (std::size_t i = 0; i < fields.size(); ++i) {
        grid->addWidget(ui::captionLabel(QStringLiteral("%1 (%2)").arg(qs(fields[i].titleRu), qs(fields[i].key)), page),
                        0, static_cast<int>(i) + 1);
    }
    grid->addWidget(ui::captionLabel(QStringLiteral("Прочитано из модуля"), page), 0,
                    static_cast<int>(fields.size()) + 1);
    for (int n = 1; n <= ps::kMixerChannels; ++n) {
        auto& r = m_mixer[static_cast<std::size_t>(n - 1)];
        r.send = new QCheckBox(QStringLiteral("Канал %1").arg(n), page);
        r.send->setObjectName(QStringLiteral("psMixer%1_send").arg(n));
        grid->addWidget(r.send, n, 0);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            r.editors[i] = makeEditor(fields[i], QStringLiteral("psMixer%1_%2").arg(n).arg(qs(fields[i].key)), r.send);
            grid->addWidget(r.editors[i], n, static_cast<int>(i) + 1);
        }
        r.actual = new QLabel(QStringLiteral("—"), page);
        r.actual->setProperty("role", QStringLiteral("hint"));
        grid->addWidget(r.actual, n, static_cast<int>(fields.size()) + 1);
    }
    grid->setColumnStretch(static_cast<int>(fields.size()) + 1, 1);
    grid->setRowStretch(ps::kMixerChannels + 1, 1);

    auto& mod = m_ui[Mixer];
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(ui::kInnerGap);
    mod.write = new QPushButton(QStringLiteral("Записать"), page);
    mod.write->setObjectName(QStringLiteral("psMixerWrite"));
    ui::setButtonRole(mod.write, ui::ButtonRole::Primary);
    mod.read = new QPushButton(QStringLiteral("Прочитать"), page);
    mod.read->setObjectName(QStringLiteral("psMixerRead"));
    ui::setButtonRole(mod.read, ui::ButtonRole::Secondary);
    mod.state = new QLabel(QStringLiteral("Ответ: —"), page);
    mod.state->setObjectName(QStringLiteral("psMixerState"));
    mod.state->setProperty("role", QStringLiteral("hint"));
    buttons->addWidget(mod.write);
    buttons->addWidget(mod.read);
    buttons->addWidget(mod.state, 1);
    connect(mod.write, &QPushButton::clicked, this, [this] { write(Mixer); });
    connect(mod.read, &QPushButton::clicked, this, [this] { read(Mixer); });

    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kGap);
    layout->addWidget(ui::hintLabel(QStringLiteral("В отмеченных каналах передаются все четыре поля."), page));
    layout->addLayout(grid);
    layout->addStretch(1);
    layout->addLayout(buttons);
    return page;
}

void PsModulesPage::setAllowed(bool allowed)
{
    for (auto& mod : m_ui) {
        mod.write->setEnabled(allowed);
        mod.read->setEnabled(allowed);
    }
}

void PsModulesPage::sendCommand(Module m, std::string_view type, const json& payload)
{
    const int count = m_link->sendCommand(type, payload);
    if (count < 0) {
        m_ui[m].state->setText(QStringLiteral("Не отправлено: нет связи с PS"));
        return;
    }
    m_pending[count] = m;
    m_ui[m].state->setText(QStringLiteral("%1 #%2 отправлена, ждём ответ…").arg(qs(type)).arg(count));
}

bool PsModulesPage::write(Module m)
{
    json payload;
    std::string error;
    bool ok = false;
    std::string_view type;
    if (m == Mixer) {
        std::vector<ps::MixerChannel> channels;
        const auto& fields = ps::mixerChannelFields();
        for (int n = 1; n <= ps::kMixerChannels; ++n) {
            const auto& r = m_mixer[static_cast<std::size_t>(n - 1)];
            if (!r.send->isChecked()) {
                continue;
            }
            ps::MixerChannel c{n, {}};
            for (std::size_t i = 0; i < fields.size(); ++i) {
                c.values[std::string(fields[i].key)] = editorValue(fields[i], r.editors[i]);
            }
            channels.push_back(std::move(c));
        }
        type = ps::msg::SetMixer;
        ok = !channels.empty() && ps::mixerSetPayload(channels, payload, &error);
        if (channels.empty()) {
            error = "не отмечено ни одного канала";
        }
    } else {
        ps::ModuleValues values;
        for (const auto& r : m_ui[m].rows) {
            if (r.send->isChecked()) {
                values[std::string(r.field.key)] = editorValue(r.field, r.editor);
            }
        }
        if (values.empty()) {
            error = "не отмечено ни одного поля";
        } else if (m == Video) {
            type = ps::msg::SetVideo;
            ok = ps::videoSetPayload(values, payload, &error);
        } else {
            type = ps::msg::SetLo;
            ok = ps::loSetPayload(m == Lo1 ? ps::kLoMixer : ps::kLoCalibration, values, payload, &error);
        }
    }
    if (!ok) {
        m_ui[m].state->setText(QStringLiteral("Не отправлено: ") + QString::fromStdString(error));
        return false;
    }
    if (!m_link->isReady()) {
        m_ui[m].state->setText(QStringLiteral("Не отправлено: нет связи с PS"));
        return false;
    }
    sendCommand(m, type, payload);
    return true;
}

bool PsModulesPage::read(Module m)
{
    if (!m_link->isReady()) {
        m_ui[m].state->setText(QStringLiteral("Не отправлено: нет связи с PS"));
        return false;
    }
    switch (m) {
    case Lo1: sendCommand(m, ps::msg::GetLo, ps::loGetPayload(ps::kLoMixer)); break;
    case Lo2: sendCommand(m, ps::msg::GetLo, ps::loGetPayload(ps::kLoCalibration)); break;
    case Video: sendCommand(m, ps::msg::GetVideo, json::object()); break;
    case Mixer: sendCommand(m, ps::msg::GetMixer, json::object()); break;
    case ModuleCount: return false;
    }
    return true;
}

void PsModulesPage::onResponse(const ps::Envelope& r, ps::Status status)
{
    // Ответы на команды страницы «Пакет» и приёмки тоже показываем: это состояние модуля.
    Module m = ModuleCount;
    if (const auto it = m_pending.find(r.msg_count); it != m_pending.end()) {
        m = it->second;
        m_pending.erase(it);
    }
    const auto& t = r.msg_type;
    if (t == ps::msg::SetMixer || t == ps::msg::GetMixer) {
        showMixer(status, r.payload);
        return;
    }
    if (t == ps::msg::SetVideo || t == ps::msg::GetVideo) {
        showModule(Video, status, r.payload);
        return;
    }
    if (t != ps::msg::SetLo && t != ps::msg::GetLo) {
        return;
    }
    if (m != Lo1 && m != Lo2) {
        // Не наша команда: модуль — по addr ответа, у отказа его может не быть.
        const auto addr = r.payload.find("addr");
        if (addr == r.payload.end() || !addr->is_number_integer()) {
            return;
        }
        m = *addr == ps::kLoMixer ? Lo1 : *addr == ps::kLoCalibration ? Lo2 : ModuleCount;
    }
    if (m != ModuleCount) {
        showModule(m, status, r.payload);
    }
}

void PsModulesPage::showModule(Module m, ps::Status status, const json& payload)
{
    auto& mod = m_ui[m];
    mod.state->setText(stateText(status, payload));
    if (status != ps::Status::Ok) {
        return;
    }
    m_filling = true;
    for (auto& r : mod.rows) {
        const auto it = payload.find(r.field.key);
        if (it == payload.end() || !it->is_number()) {
            continue;
        }
        const double v = it->get<double>();
        r.actual->setText(valueText(r.field, v));
        setEditorValue(r.field, r.editor, v);
    }
    m_filling = false;
}

void PsModulesPage::showMixer(ps::Status status, const json& payload)
{
    m_ui[Mixer].state->setText(stateText(status, payload));
    if (status != ps::Status::Ok) {
        return;
    }
    ps::MixerState state;
    std::string error;
    if (!ps::MixerState::fromJson(payload, state, &error)) {
        m_ui[Mixer].state->setText(QStringLiteral("Ответ не разобран: ") + QString::fromStdString(error));
        return;
    }
    const auto& fields = ps::mixerChannelFields();
    m_filling = true;
    for (const auto& c : state.ch) {
        auto& r = m_mixer[static_cast<std::size_t>(c.n - 1)];
        QStringList parts;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto v = c.values.find(std::string(fields[i].key));
            if (v == c.values.end()) {
                continue;
            }
            parts << QStringLiteral("%1=%2").arg(qs(fields[i].key), valueText(fields[i], v->second));
            setEditorValue(fields[i], r.editors[i], v->second);
        }
        r.actual->setText(parts.join(QStringLiteral(", ")));
    }
    m_filling = false;
}
