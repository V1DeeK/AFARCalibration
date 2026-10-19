#include "PsOsdPage.h"

#include "PsLink.h"
#include "PsSession.h"
#include "UiStyle.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

QSpinBox* offsetSpin(QWidget* parent, const QString& name, const QString& suffix)
{
    auto* spin = new QSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(ps::kOsdOffsetMin, ps::kOsdOffsetMax);
    spin->setSuffix(suffix);
    return spin;
}

}  // namespace

PsOsdPage::PsOsdPage(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kInnerGap);

    auto* params = new QGroupBox(QStringLiteral("Параметры OSD"), this);
    auto* grid = new QGridLayout(params);
    grid->setHorizontalSpacing(ui::kInnerGap);
    m_en = new QCheckBox(QStringLiteral("OSD включён"), params);
    m_en->setObjectName(QStringLiteral("psOsdEn"));
    m_en->setChecked(true);
    m_std = new QComboBox(params);
    m_std->setObjectName(QStringLiteral("psOsdStd"));
    m_std->addItem(QStringLiteral("PAL"), QStringLiteral("PAL"));
    m_std->addItem(QStringLiteral("NTSC"), QStringLiteral("NTSC"));
    m_src = new QComboBox(params);
    m_src->setObjectName(QStringLiteral("psOsdSrc"));
    m_src->addItem(QStringLiteral("Автоматически (AUTO)"), QStringLiteral("AUTO"));
    m_src->addItem(QStringLiteral("Отладочный текст (TEXT)"), QStringLiteral("TEXT"));
    m_hos = offsetSpin(params, QStringLiteral("psOsdHos"), QStringLiteral(" пикс."));
    m_vos = offsetSpin(params, QStringLiteral("psOsdVos"), QStringLiteral(" строк"));
    m_los = new QLabel(QStringLiteral("видеосигнал: —"), params);
    m_los->setObjectName(QStringLiteral("psOsdLos"));
    m_los->setProperty("role", QStringLiteral("hint"));

    grid->addWidget(m_en, 0, 0, 1, 2);
    grid->addWidget(ui::captionLabel(QStringLiteral("Видеостандарт"), params), 1, 0);
    grid->addWidget(m_std, 1, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("Источник"), params), 2, 0);
    grid->addWidget(m_src, 2, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("Смещение по горизонтали"), params), 3, 0);
    grid->addWidget(m_hos, 3, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("Смещение по вертикали"), params), 4, 0);
    grid->addWidget(m_vos, 4, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("Поля при источнике AUTO"), params), 0, 2);
    int row = 1;
    for (std::string_view id : ps::osdFieldIds()) {
        auto* box = new QCheckBox(QString::fromStdString(ps::osdFieldRu(id)), params);
        box->setObjectName(QStringLiteral("psOsdField_%1").arg(qs(id)));
        box->setToolTip(qs(id));
        box->setChecked(true);
        grid->addWidget(box, row++, 2);
        m_fields.push_back(box);
    }
    grid->setColumnStretch(3, 1);

    m_write = new QPushButton(QStringLiteral("Записать OSD"), params);
    m_write->setObjectName(QStringLiteral("psSetOsd"));
    m_write->setToolTip(QStringLiteral("set_osd"));
    m_read = new QPushButton(QStringLiteral("Прочитать OSD"), params);
    m_read->setObjectName(QStringLiteral("psGetOsd"));
    m_read->setToolTip(QStringLiteral("get_osd"));
    ui::setButtonRole(m_write, ui::ButtonRole::Primary);
    ui::setButtonRole(m_read, ui::ButtonRole::Secondary);
    auto* paramButtons = new QHBoxLayout;
    paramButtons->addWidget(m_los, 1);
    paramButtons->addWidget(m_read);
    paramButtons->addWidget(m_write);
    grid->addLayout(paramButtons, 6, 0, 1, 4);

    auto* text = new QGroupBox(QStringLiteral("Отладочный текст (режим SERVICE)"), this);
    auto* textGrid = new QGridLayout(text);
    textGrid->setHorizontalSpacing(ui::kInnerGap);
    m_row = new QSpinBox(text);
    m_row->setObjectName(QStringLiteral("psOsdRow"));
    m_col = new QSpinBox(text);
    m_col->setObjectName(QStringLiteral("psOsdCol"));
    m_col->setRange(0, ps::kOsdColumns - 1);
    m_text = new QLineEdit(text);
    m_text->setObjectName(QStringLiteral("psOsdText"));
    m_text->setMaxLength(ps::kOsdColumns);
    m_text->setPlaceholderText(QStringLiteral("латиница, цифры и знаки ASCII"));
    m_text->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[\\x20-\\x7E]*")), m_text));
    m_writeText = new QPushButton(QStringLiteral("Вывести текст"), text);
    m_writeText->setObjectName(QStringLiteral("psSetOsdText"));
    m_writeText->setToolTip(QStringLiteral("set_osd_text"));
    m_clearText = new QPushButton(QStringLiteral("Очистить экран"), text);
    m_clearText->setObjectName(QStringLiteral("psClearOsdText"));
    m_clearText->setToolTip(QStringLiteral("clear_osd_text"));
    ui::setButtonRole(m_writeText, ui::ButtonRole::Primary);
    ui::setButtonRole(m_clearText, ui::ButtonRole::Secondary);
    textGrid->addWidget(ui::captionLabel(QStringLiteral("Строка"), text), 0, 0);
    textGrid->addWidget(ui::captionLabel(QStringLiteral("Столбец"), text), 0, 1);
    textGrid->addWidget(ui::captionLabel(QStringLiteral("Текст, до 30 символов вместе со столбцом"), text), 0, 2);
    textGrid->addWidget(m_row, 1, 0);
    textGrid->addWidget(m_col, 1, 1);
    textGrid->addWidget(m_text, 1, 2);
    textGrid->setColumnStretch(2, 1);
    auto* textButtons = new QHBoxLayout;
    textButtons->addWidget(ui::hintLabel(QStringLiteral("Текст виден на экране, когда источник OSD — TEXT."), text), 1);
    textButtons->addWidget(m_clearText);
    textButtons->addWidget(m_writeText);
    textGrid->addLayout(textButtons, 2, 0, 1, 3);

    m_result = new QLabel(QStringLiteral("OSD формирует MAX7456 видеоприёмника: 16 строк по 30 символов (PAL)."), this);
    m_result->setObjectName(QStringLiteral("psOsdResult"));
    m_result->setWordWrap(true);

    layout->addWidget(params);
    layout->addWidget(text);
    layout->addWidget(m_result);
    layout->addStretch(1);

    updateRowLimit();
    connect(m_std, &QComboBox::currentIndexChanged, this, &PsOsdPage::updateRowLimit);
    connect(m_write, &QPushButton::clicked, this, &PsOsdPage::writeOsd);
    connect(m_read, &QPushButton::clicked, this, &PsOsdPage::readOsd);
    connect(m_writeText, &QPushButton::clicked, this, &PsOsdPage::writeText);
    connect(m_clearText, &QPushButton::clicked, this, &PsOsdPage::clearText);
    connect(m_session->link(), &PsLink::responseReceived, this, &PsOsdPage::onResponse);
    connect(m_session->link(), &PsLink::commandTimedOut, this, [this](const QString& type, int count) {
        if (m_pending.erase(count) > 0) {
            m_result->setText(QStringLiteral("%1: PS не ответил").arg(type));
        }
    });
    setAllowed(false, false);
}

void PsOsdPage::setAllowed(bool connected, bool service)
{
    m_write->setEnabled(connected);
    m_read->setEnabled(connected);
    m_writeText->setEnabled(connected && service);
    m_clearText->setEnabled(connected && service);
}

ps::OsdParams PsOsdPage::formParams() const
{
    ps::OsdParams p;
    p.en = m_en->isChecked() ? 1 : 0;
    p.standard = m_std->currentData().toString().toStdString();
    p.src = m_src->currentData().toString().toStdString();
    std::vector<std::string> fields;
    for (std::size_t i = 0; i < m_fields.size(); ++i) {
        if (m_fields[i]->isChecked()) {
            fields.emplace_back(ps::osdFieldIds()[i]);
        }
    }
    p.fields = std::move(fields);
    p.hos = m_hos->value();
    p.vos = m_vos->value();
    return p;
}

bool PsOsdPage::sendTracked(std::string_view type, const json& payload)
{
    const int count = m_session->send(type, payload);
    if (count < 0) {
        m_result->setText(m_session->lastError());
        return false;
    }
    m_pending[count] = std::string(type);
    m_result->setText(QStringLiteral("%1: отправлено").arg(qs(type)));
    return true;
}

bool PsOsdPage::writeOsd()
{
    const ps::OsdParams p = formParams();
    std::string error;
    if (!p.validate(&error)) {
        m_result->setText(QString::fromStdString(error));
        return false;
    }
    return sendTracked(ps::msg::SetOsd, p.toJson());
}

bool PsOsdPage::readOsd()
{
    return sendTracked(ps::msg::GetOsd, json::object());
}

bool PsOsdPage::writeText()
{
    json payload;
    std::string error;
    if (!ps::osdTextPayload(m_row->value(), m_col->value(), m_text->text().toStdString(),
                            m_std->currentData().toString() == QStringLiteral("NTSC"), payload, &error)) {
        m_result->setText(QString::fromStdString(error));
        return false;
    }
    return sendTracked(ps::msg::SetOsdText, payload);
}

bool PsOsdPage::clearText()
{
    return sendTracked(ps::msg::ClearOsdText, json::object());
}

void PsOsdPage::updateRowLimit()
{
    const bool ntsc = m_std->currentData().toString() == QStringLiteral("NTSC");
    m_row->setRange(0, (ntsc ? ps::kOsdRowsNtsc : ps::kOsdRowsPal) - 1);
}

void PsOsdPage::showParams(const ps::OsdParams& p)
{
    if (p.en) m_en->setChecked(*p.en != 0);
    if (p.standard) {
        const int i = m_std->findData(QString::fromStdString(*p.standard));
        if (i >= 0) m_std->setCurrentIndex(i);
    }
    if (p.src) {
        const int i = m_src->findData(QString::fromStdString(*p.src));
        if (i >= 0) m_src->setCurrentIndex(i);
    }
    if (p.fields) {
        for (std::size_t i = 0; i < m_fields.size(); ++i) {
            const std::string id(ps::osdFieldIds()[i]);
            m_fields[i]->setChecked(std::find(p.fields->begin(), p.fields->end(), id) != p.fields->end());
        }
    }
    if (p.hos) m_hos->setValue(*p.hos);
    if (p.vos) m_vos->setValue(*p.vos);
    if (p.los) {
        m_los->setText(*p.los == 0 ? QStringLiteral("видеосигнал есть") : QStringLiteral("видеосигнала нет"));
        ui::setTone(m_los, *p.los == 0 ? ui::Tone::Ok : ui::Tone::Warn);
    }
}

void PsOsdPage::onResponse(const ps::Envelope& r, ps::Status status)
{
    const auto it = m_pending.find(r.msg_count);
    if (it == m_pending.end() || it->second != r.msg_type) {
        return;
    }
    m_pending.erase(it);
    const QString type = qs(r.msg_type);
    if (status != ps::Status::Ok) {
        m_result->setText(QStringLiteral("%1: PS ответил %2 (%3)")
                              .arg(type, qs(ps::toString(status)), QString::fromStdString(ps::statusRu(status))));
        return;
    }
    if (r.msg_type == ps::msg::SetOsd || r.msg_type == ps::msg::GetOsd) {
        ps::OsdParams p;
        std::string error;
        if (!ps::OsdParams::fromJson(r.payload, p, &error)) {
            m_result->setText(QStringLiteral("%1: ответ не разобран — %2").arg(type, QString::fromStdString(error)));
            return;
        }
        showParams(p);
        m_result->setText(r.msg_type == ps::msg::SetOsd ? QStringLiteral("OSD записан, PS вернул текущие параметры")
                                                        : QStringLiteral("Параметры OSD прочитаны"));
    } else if (r.msg_type == ps::msg::SetOsdText) {
        m_result->setText(QStringLiteral("Текст выведен на экран OSD"));
    } else {
        m_result->setText(QStringLiteral("Экран OSD очищен"));
    }
}
