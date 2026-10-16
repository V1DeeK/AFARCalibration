#include "PsPacketPage.h"

#include "PsSession.h"
#include "UiStyle.h"

#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
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
    if (type == ps::msg::SetBeamAzimuth) {
        return json{{"az", 0.0}};
    }
    if (type == ps::msg::SetBeam) {
        return json{{"beam", 0}};
    }
    if (type == ps::msg::Rs485Send) {
        return json{{"data", "01A3FF"}};
    }
    if (type == ps::msg::SetSffParameters) {
        return ps::SffParams{}.toJson();
    }
    // Модули (§4.5): образцы из v3 §2.5.2.
    if (type == ps::msg::SetLo) {
        return json{{"addr", ps::kLoMixer}, {"pwr_b", 5},  {"pwr_a", 5},  {"en_b", 1},
                    {"en_a", 1},           {"en_rf", 1},  {"en_syn", 1}, {"ref", 0},
                    {"freq", 5'500'000'000LL}, {"att1", 0.5}, {"att2", 31.5}};
    }
    if (type == ps::msg::GetLo) {
        return ps::loGetPayload(ps::kLoMixer);
    }
    if (type == ps::msg::SetMixer) {
        return json{{"ch", json::array({json{{"n", 1}, {"en", 1}, {"rp", 0}, {"if_att", 2.5}, {"att", 10.0}},
                                        json{{"n", 2}, {"en", 0}}})}};
    }
    if (type == ps::msg::SetVideo) {
        return json{{"en_vid", 1}, {"att_mode", 1}, {"freq", 5'800'000'000LL}, {"att1", 6.0}};
    }
    return json::object();
}

QString pretty(const QString& line)
{
    const json j = json::parse(line.toStdString(), nullptr, false);
    return j.is_discarded() ? line : QString::fromStdString(j.dump(2));
}

}  // namespace

PsPacketPage::PsPacketPage(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kInnerGap);
    auto* head = new QHBoxLayout;
    head->setSpacing(ui::kInnerGap);
    m_type = new QComboBox(this);
    m_type->setObjectName(QStringLiteral("psPacketType"));
    for (std::string_view t :
         {ps::msg::GetState, ps::msg::SetMode, ps::msg::SetSffParameters, ps::msg::GetSffParameters,
          ps::msg::SetSffStart, ps::msg::SetSffStop, ps::msg::SetPhaseAtten, ps::msg::GetPhaseAtten,
          ps::msg::GetRfmTelemetry, ps::msg::GetTrxStatus, ps::msg::GetPlStatus,
          ps::msg::SetBeamAzimuth, ps::msg::SetBeam, ps::msg::GetBeam,
          ps::msg::Rs485Send, ps::msg::SetLo, ps::msg::GetLo, ps::msg::SetMixer, ps::msg::GetMixer,
          ps::msg::SetVideo, ps::msg::GetVideo}) {
        m_type->addItem(qs(t));
    }
    m_type->setEditable(true);  // можно ввести и msg_type, которого нет в контракте
    auto* reset = new QPushButton(QStringLiteral("Шаблон"), this);
    m_send = new QPushButton(QStringLiteral("Отправить"), this);
    m_send->setObjectName(QStringLiteral("psPacketSend"));
    auto* clear = new QPushButton(QStringLiteral("Очистить историю"), this);
    ui::setButtonRole(m_send, ui::ButtonRole::Primary);
    ui::setButtonRole(reset, ui::ButtonRole::Secondary);
    ui::setButtonRole(clear, ui::ButtonRole::Secondary);
    head->addWidget(new QLabel(QStringLiteral("msg_type"), this));
    head->addWidget(m_type, 1);
    head->addWidget(reset);
    head->addWidget(m_send);
    head->addWidget(clear);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_payload = new QPlainTextEdit(this);
    m_payload->setObjectName(QStringLiteral("psPacketPayload"));
    m_payload->setPlaceholderText(QStringLiteral("payload — объект JSON, можно править перед отправкой"));
    m_payload->setFont(mono);
    m_payload->setMaximumHeight(110);
    m_result = new QLabel(QStringLiteral("Выберите команду и нажмите «Отправить»."), this);
    m_result->setObjectName(QStringLiteral("psPacketResult"));
    m_result->setProperty("role", QStringLiteral("hint"));
    m_result->setWordWrap(true);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    m_history = new QTableWidget(0, 4, split);
    m_history->setObjectName(QStringLiteral("psPacketHistory"));
    m_history->setHorizontalHeaderLabels({QStringLiteral("Время"), QStringLiteral("Команда"),
                                          QStringLiteral("№"), QStringLiteral("Результат")});
    m_history->verticalHeader()->setVisible(false);
    m_history->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_history->setSelectionMode(QAbstractItemView::SingleSelection);
    m_history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_history->setAlternatingRowColors(true);
    m_history->setShowGrid(false);
    m_history->setMinimumHeight(120);
    // Время и № — по содержимому, «Команда» и «Результат» делят остальную ширину поровну.
    m_history->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_history->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_history->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    auto* packets = new QWidget(split);
    auto* pl = new QVBoxLayout(packets);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(2);
    m_request = new QPlainTextEdit(packets);
    m_request->setObjectName(QStringLiteral("psPacketRequest"));
    m_response = new QPlainTextEdit(packets);
    m_response->setObjectName(QStringLiteral("psPacketResponse"));
    for (QPlainTextEdit* e : {m_request, m_response}) {
        e->setReadOnly(true);
        e->setFont(mono);
    }
    m_request->setPlaceholderText(QStringLiteral("здесь будет пакет, который ушёл в PS"));
    m_response->setPlaceholderText(QStringLiteral("здесь будет ответ PS"));
    pl->addWidget(ui::captionLabel(QStringLiteral("Отправлено"), packets));
    pl->addWidget(m_request, 1);
    pl->addWidget(ui::captionLabel(QStringLiteral("Получено"), packets));
    pl->addWidget(m_response, 1);
    split->addWidget(m_history);
    split->addWidget(packets);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);

    layout->addLayout(head);
    layout->addWidget(m_payload);
    layout->addWidget(m_result);
    layout->addWidget(split, 1);

    PsLink* link = m_session->link();
    connect(m_type, &QComboBox::currentIndexChanged, this, [this](int) { loadTemplate(); });
    connect(reset, &QPushButton::clicked, this, &PsPacketPage::loadTemplate);
    connect(m_send, &QPushButton::clicked, this, &PsPacketPage::sendPacket);
    connect(clear, &QPushButton::clicked, this, [this] {
        m_exchanges.clear();
        m_history->setRowCount(0);
        m_request->clear();
        m_response->clear();
    });
    connect(m_history, &QTableWidget::currentCellChanged, this, [this](int row) {
        if (row >= 0) {
            showExchange(static_cast<int>(m_exchanges.size()) - 1 - row);
        }
    });
    connect(link, &PsLink::frameLogged, this, &PsPacketPage::onFrame);
    connect(link, &PsLink::commandTimedOut, this, &PsPacketPage::onTimeout);
    connect(link, &PsLink::stateChanged, this, &PsPacketPage::onLinkState);
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
    const json payload = text.empty() ? json::object() : json::parse(text, nullptr, false);
    Exchange e;
    e.msgType = QString::fromStdString(type);
    e.sentAt = QDateTime::currentDateTime();
    e.clock.start();
    if (type.empty() || payload.is_discarded() || !payload.is_object()) {
        m_result->setText(QStringLiteral("Не отправлено: payload должен быть объектом JSON, msg_type — не пустым."));
        return false;
    }
    if (type == ps::msg::Hello || type == ps::msg::KeepAlive) {
        m_result->setText(QStringLiteral("hello и keep_alive отправляет само соединение."));
        return false;
    }
    const int count = m_session->send(type, payload);
    if (count < 0) {
        e.result = Result::NotSent;
        e.note = m_session->lastError();
        e.request = QString::fromStdString(ps::encodeLine(ps::makeCommand(type, 0, payload))).trimmed();
        addExchange(e);
        m_result->setText(QStringLiteral("Не отправлено: нет связи с PS."));
        return false;
    }
    e.msgCount = count;
    e.request = QString::fromStdString(
                    ps::encodeLine(ps::makeCommand(type, static_cast<std::uint8_t>(count), payload)))
                    .trimmed();
    addExchange(e);
    m_result->setText(QStringLiteral("%1 №%2 отправлена, ждём ответ…").arg(e.msgType).arg(count));
    return true;
}

void PsPacketPage::addExchange(const Exchange& e)
{
    m_exchanges.push_back(e);
    m_history->insertRow(0);
    refreshRow(static_cast<int>(m_exchanges.size()) - 1);
    m_history->setCurrentCell(0, 0);
    showExchange(static_cast<int>(m_exchanges.size()) - 1);
}

int PsPacketPage::findExchange(const QString& msgType, int msgCount) const
{
    for (int i = static_cast<int>(m_exchanges.size()) - 1; i >= 0; --i) {
        if (m_exchanges.at(i).msgType == msgType && m_exchanges.at(i).msgCount == msgCount) {
            return i;
        }
    }
    return -1;
}

void PsPacketPage::onFrame(bool outgoing, const QString& channel, const QString& line)
{
    if (outgoing || channel != QLatin1String("TCP") || m_exchanges.isEmpty()) {
        return;
    }
    ps::Envelope env;
    if (!ps::decodeLine(line.toStdString(), env) || env.msg_class != ps::MsgClass::Response) {
        return;
    }
    const int i = findExchange(QString::fromStdString(env.msg_type), env.msg_count);
    if (i < 0) {
        return;  // чужой обмен (кнопки вкладок, keep_alive) — не показываем
    }
    Exchange& e = m_exchanges[i];
    e.response = line.trimmed();
    e.answerMs = e.clock.elapsed();
    ps::Status status{};
    if (ps::responseStatus(env, status)) {
        e.result = status == ps::Status::Ok ? Result::Ok : Result::Refused;
        e.note = qs(ps::toString(status));
    } else {
        e.result = Result::Refused;
        e.note = QStringLiteral("в ответе нет корректного status");
    }
    m_result->setText(QStringLiteral("%1 №%2: %3").arg(e.msgType).arg(e.msgCount).arg(resultText(e)));
    refreshRow(i);
    if (m_history->currentRow() == static_cast<int>(m_exchanges.size()) - 1 - i) {
        showExchange(i);
    }
}

void PsPacketPage::onTimeout(const QString& msgType, int msgCount)
{
    const int i = findExchange(msgType, msgCount);
    if (i < 0 || m_exchanges.at(i).result != Result::Waiting) {
        return;
    }
    m_exchanges[i].result = Result::Timeout;
    refreshRow(i);
    showExchange(i);
}

void PsPacketPage::onLinkState()
{
    if (m_session->link()->isReady()) {
        return;
    }
    for (int i = 0; i < m_exchanges.size(); ++i) {
        if (m_exchanges.at(i).result == Result::Waiting) {
            m_exchanges[i].result = Result::LinkLost;
            refreshRow(i);
        }
    }
}

QString PsPacketPage::resultText(const Exchange& e) const
{
    const int limitMs = m_session->link()->timeouts().commandMs;
    switch (e.result) {
    case Result::Waiting:
        return QStringLiteral("ждём ответ…");
    case Result::Ok:
        return QStringLiteral("ответ OK за %1 мс").arg(e.answerMs);
    case Result::Refused:
        return QStringLiteral("ответ получен, PS отказал: %1").arg(e.note);
    case Result::Timeout:
        return QStringLiteral("ответа нет: за %1 мс PS не ответил").arg(limitMs);
    case Result::LinkLost:
        return QStringLiteral("связь с PS прервалась, ответа нет");
    case Result::NotSent:
        return QStringLiteral("не отправлено: %1").arg(e.note);
    }
    return {};
}

void PsPacketPage::refreshRow(int index)
{
    const Exchange& e = m_exchanges.at(index);
    const int row = static_cast<int>(m_exchanges.size()) - 1 - index;
    const QStringList cells{e.sentAt.toString(QStringLiteral("HH:mm:ss")), e.msgType,
                            e.msgCount < 0 ? QStringLiteral("—") : QString::number(e.msgCount),
                            resultText(e)};
    for (int c = 0; c < cells.size(); ++c) {
        auto* item = m_history->item(row, c);
        if (!item) {
            item = new QTableWidgetItem;
            m_history->setItem(row, c, item);
        }
        item->setText(cells.at(c));
        if (c == 3 && e.result != Result::Ok && e.result != Result::Waiting) {
            item->setForeground(QBrush(ui::errorColor()));
        }
    }
}

void PsPacketPage::showExchange(int index)
{
    if (index < 0 || index >= m_exchanges.size()) {
        return;
    }
    const Exchange& e = m_exchanges.at(index);
    m_request->setPlainText(e.result == Result::NotSent
                                ? QStringLiteral("ПАКЕТ НЕ УШЁЛ В PS. Так он выглядел бы:\n\n") + pretty(e.request)
                                : pretty(e.request));
    m_response->setPlainText(e.response.isEmpty() ? QString() : pretty(e.response));
    m_response->setPlaceholderText(e.result == Result::Waiting ? QStringLiteral("ждём ответ PS…")
                                                               : QStringLiteral("ответа нет"));
}

void PsPacketPage::loadTemplate()
{
    m_payload->setPlainText(QString::fromStdString(templateFor(m_type->currentText().toStdString()).dump(2)));
}
