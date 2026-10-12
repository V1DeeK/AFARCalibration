#include "PsPacketPage.h"

#include "PsLink.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

/// Шаблон payload по контракту (§3–4); безопасные значения.
json templateFor(std::string_view type)
{
    if (type == ps::msg::SetMode) {
        return json{{"mode", "SERVICE"}};
    }
    if (type == ps::msg::SetPhaseAtten) {
        std::vector<ps::ChannelCodes> codes;
        for (int n = 1; n <= ps::kChannels; ++n) {
            codes.push_back({n, 0, ps::kCodeMax});
        }
        json p;
        ps::channelsToJson(codes, p);
        return p;
    }
    if (type == ps::msg::SetAdc) {
        return ps::AdcParams{5'800'000'000, 20'000'000, 30'720'000, 40.0}.toJson();
    }
    if (type == ps::msg::SetBeamAzimuth) {
        return json{{"az", 0.0}};
    }
    if (type == ps::msg::SetBeam) {
        return json{{"beam", 0}};
    }
    if (type == ps::msg::Rs485Send) {
        return json{{"data", "01A3FF"}};
    }
    if (type == ps::msg::Rs485Set) {
        return json{{"addr", 1}, {"EN_ADF4355", 0}};
    }
    if (type == ps::msg::Rs485Status) {
        return ps::rs485StatusPayload(ps::Rs485Module::MixerLo);
    }
    return json::object();
}

}  // namespace

PsPacketPage::PsPacketPage(PsLink* link, QWidget* parent)
    : QWidget(parent)
    , m_link(link)
{
    auto* layout = new QVBoxLayout(this);
    auto* head = new QHBoxLayout;
    m_type = new QComboBox(this);
    m_type->setObjectName(QStringLiteral("psPacketType"));
    for (std::string_view t : {ps::msg::GetState, ps::msg::SetMode, ps::msg::SetPhaseAtten, ps::msg::GetPhaseAtten,
                               ps::msg::SetAdc, ps::msg::GetAdc, ps::msg::SetBeamAzimuth, ps::msg::SetBeam,
                               ps::msg::GetBeam, ps::msg::Rs485Send, ps::msg::Rs485Set, ps::msg::Rs485Status}) {
        m_type->addItem(qs(t));
    }
    m_type->setEditable(true);  // можно ввести и msg_type, которого нет в контракте
    auto* reset = new QPushButton(QStringLiteral("Шаблон"), this);
    m_send = new QPushButton(QStringLiteral("Отправить"), this);
    m_send->setObjectName(QStringLiteral("psPacketSend"));
    head->addWidget(new QLabel(QStringLiteral("msg_type"), this));
    head->addWidget(m_type, 1);
    head->addWidget(reset);
    head->addWidget(m_send);

    m_payload = new QPlainTextEdit(this);
    m_payload->setObjectName(QStringLiteral("psPacketPayload"));
    m_payload->setPlaceholderText(QStringLiteral("payload — объект JSON"));
    m_result = new QLabel(QStringLiteral("Ответ PS — в журнале обмена ниже."), this);
    m_result->setObjectName(QStringLiteral("psPacketResult"));
    m_result->setWordWrap(true);

    layout->addLayout(head);
    layout->addWidget(m_payload, 1);
    layout->addWidget(m_result);

    connect(m_type, &QComboBox::currentIndexChanged, this, [this](int) { loadTemplate(); });
    connect(reset, &QPushButton::clicked, this, &PsPacketPage::loadTemplate);
    connect(m_send, &QPushButton::clicked, this, &PsPacketPage::sendPacket);
    connect(m_link, &PsLink::responseReceived, this, [this](const ps::Envelope& r, ps::Status status) {
        if (r.msg_type == m_type->currentText().toStdString()) {
            m_result->setText(QStringLiteral("%1 #%2: %3")
                                  .arg(qs(r.msg_type))
                                  .arg(r.msg_count)
                                  .arg(qs(ps::toString(status))));
        }
    });
    loadTemplate();
    setAllowed(false);
}

void PsPacketPage::setAllowed(bool allowed)
{
    m_send->setEnabled(allowed);
}

bool PsPacketPage::sendPacket()
{
    const std::string type = m_type->currentText().trimmed().toStdString();
    const std::string text = m_payload->toPlainText().trimmed().toStdString();
    json payload = text.empty() ? json::object() : json::parse(text, nullptr, false);
    if (type.empty() || payload.is_discarded() || !payload.is_object()) {
        m_result->setText(QStringLiteral("Не отправлено: payload должен быть объектом JSON, msg_type — не пустым."));
        return false;
    }
    if (type == ps::msg::Hello || type == ps::msg::KeepAlive) {
        m_result->setText(QStringLiteral("hello и keep_alive отправляет само соединение."));
        return false;
    }
    const int count = m_link->sendCommand(type, payload);
    if (count < 0) {
        m_result->setText(QStringLiteral("Не отправлено: нет связи с PS."));
        return false;
    }
    m_result->setText(QStringLiteral("%1 #%2 отправлена, ждём ответ…").arg(QString::fromStdString(type)).arg(count));
    return true;
}

void PsPacketPage::loadTemplate()
{
    m_payload->setPlainText(QString::fromStdString(templateFor(m_type->currentText().toStdString()).dump(2)));
}
