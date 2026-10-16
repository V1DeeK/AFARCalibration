#include "PsJournalDialog.h"

#include "PsExchangeView.h"
#include "PsSession.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace ps = afar::ps;
using nlohmann::json;

PsJournalDialog::PsJournalDialog(PsSession* session, QWidget* parent)
    : QDialog(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("psJournalDialog"));
    setWindowTitle(QStringLiteral("Журнал обмена с PS"));
    setModal(true);
    resize(1000, 640);

    auto* root = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    auto* page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(page);
    m_exchange = new PsExchangeView(m_session, page);
    m_exchange->table()->setObjectName(QStringLiteral("psJournalTable"));
    layout->addWidget(m_exchange, 1);

    auto* raw = new QHBoxLayout;
    m_type = new QComboBox(page);
    m_type->setEditable(true);
    for (std::string_view t : {ps::msg::GetState, ps::msg::SetMode, ps::msg::GetSffParameters,
                               ps::msg::SetSffParameters, ps::msg::SetSffStart, ps::msg::SetSffStop,
                               ps::msg::GetPhaseAtten, ps::msg::GetRfmTelemetry,
                               ps::msg::GetTrxStatus, ps::msg::GetPlStatus, ps::msg::GetBeam,
                               ps::msg::GetLo, ps::msg::GetMixer, ps::msg::GetVideo}) {
        m_type->addItem(QString::fromUtf8(t.data(), static_cast<qsizetype>(t.size())));
    }
    m_payload = new QLineEdit(page);
    m_payload->setPlaceholderText(QStringLiteral("payload JSON, например {\"mode\":\"SERVICE\"}"));
    auto* send = new QPushButton(QStringLiteral("Отправить"), page);
    ui::setButtonRole(send, ui::ButtonRole::Primary);
    raw->addWidget(m_type);
    raw->addWidget(m_payload, 1);
    raw->addWidget(send);
    layout->addLayout(raw);
    m_result = new QLabel(QStringLiteral("Ручная отправка — для проверки протокола."), page);
    m_result->setWordWrap(true);
    layout->addWidget(m_result);
    tabs->addTab(page, QStringLiteral("Журнал"));
    root->addWidget(tabs, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    ui::setButtonRole(buttons->button(QDialogButtonBox::Close), ui::ButtonRole::Secondary);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(send, &QPushButton::clicked, this, &PsJournalDialog::sendRaw);
    connect(m_payload, &QLineEdit::returnPressed, this, &PsJournalDialog::sendRaw);
}

void PsJournalDialog::sendRaw()
{
    const std::string type = m_type->currentText().trimmed().toStdString();
    const std::string text = m_payload->text().trimmed().toStdString();
    const json payload = text.empty() ? json::object() : json::parse(text, nullptr, false);
    if (type.empty() || payload.is_discarded() || !payload.is_object()) {
        m_result->setText(QStringLiteral("Не отправлено: payload должен быть объектом JSON."));
        return;
    }
    if (type == ps::msg::Hello || type == ps::msg::KeepAlive) {
        m_result->setText(QStringLiteral("hello и keep_alive отправляет само соединение."));
        return;
    }
    const int count = m_session->send(type, payload);
    m_result->setText(count < 0 ? QStringLiteral("Не отправлено: %1").arg(m_session->lastError())
                                : QStringLiteral("%1 №%2 отправлена").arg(QString::fromStdString(type)).arg(count));
}
