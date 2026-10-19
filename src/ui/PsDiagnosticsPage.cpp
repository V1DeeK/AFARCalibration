#include "PsDiagnosticsPage.h"

#include "PsLink.h"
#include "PsSession.h"
#include "UiStyle.h"

#include <QCheckBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

QString qs(std::string_view s)
{
    return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()));
}

/// Флаг ответа: подписи для 0 и 1 и какое значение считается неисправностью.
struct Flag {
    std::string_view key;
    const char* title;
    const char* off;
    const char* on;
    int alarmWhen;
};

// v8 таблица 36.
const std::vector<Flag>& trxFlags()
{
    static const std::vector<Flag> flags{
        {"spi_ok", "Обмен с трансивером по SPI", "нет обмена", "обмен есть", 0},
        {"bbpll_lock", "ФАПЧ тактирования BBPLL", "нет захвата", "захват", 0},
        {"rx_lo_lock", "ФАПЧ гетеродина приёмника", "нет захвата", "захват", 0},
        {"cal_done", "Калибровки трансивера", "не завершены", "завершены", 0},
        {"if_ok", "Интерфейс данных АЦП", "нет связи", "связь есть", 0},
        {"pn_err", "Проверочная последовательность", "ошибок нет", "ошибка", 1},
        {"adc_ovr", "Перегрузка АЦП", "не было", "была", 1},
        {"dma_ovf", "Переполнение приёмного DMA", "не было", "было, часть отсчётов потеряна", 1},
    };
    return flags;
}

// v8 таблица 37.
const std::vector<Flag>& plFlags()
{
    static const std::vector<Flag> flags{
        {"adc_overrun", "Поток данных от приёмопередатчика", "не прерывался", "прерывался", 1},
        {"fft0_error", "Кадр БПФ 0", "ошибок нет", "повреждённый кадр", 1},
        {"fft1_error", "Кадр БПФ 1", "ошибок нет", "повреждённый кадр", 1},
    };
    return flags;
}

QString title(std::string_view type)
{
    if (type == ps::msg::GetTrxStatus) return QStringLiteral("Трансивер AD9361");
    if (type == ps::msg::GetPlStatus) return QStringLiteral("ПЛИС");
    return QStringLiteral("РЧ-модули");
}

QString celsius(double v)
{
    return QStringLiteral("%1 °C").arg(v, 0, 'f', 1);
}

}  // namespace

PsDiagnosticsPage::PsDiagnosticsPage(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(ui::kGap, ui::kGap, ui::kGap, ui::kGap);
    layout->setSpacing(ui::kInnerGap);

    // Две колонки кнопок: окошко «Тех. состояние» узкое.
    auto* buttons = new QGridLayout;
    buttons->setHorizontalSpacing(ui::kInnerGap);
    buttons->setVerticalSpacing(ui::kInnerGap);
    const struct {
        std::string_view type;
        QString text;
        QString name;
    } commands[] = {
        {ps::msg::GetTrxStatus, QStringLiteral("Состояние AD9361"), QStringLiteral("psGetTrxStatus")},
        {ps::msg::GetPlStatus, QStringLiteral("Состояние ПЛИС"), QStringLiteral("psGetPlStatus")},
        {ps::msg::GetRfmTelemetry, QStringLiteral("Температуры РЧ-модулей"), QStringLiteral("psGetRfmTelemetry")},
    };
    int index = 0;
    for (const auto& c : commands) {
        auto* b = new QPushButton(c.text, this);
        b->setObjectName(c.name);
        b->setToolTip(qs(c.type));
        ui::setButtonRole(b, ui::ButtonRole::Secondary);
        const std::string type(c.type);
        connect(b, &QPushButton::clicked, this, [this, type] { request(type); });
        buttons->addWidget(b, index / 2, index % 2);
        ++index;
        m_buttons.push_back(b);
    }

    auto* pollRow = new QHBoxLayout;
    pollRow->setSpacing(ui::kInnerGap);
    m_poll = new QCheckBox(QStringLiteral("Опрос AD9361 каждые"), this);
    m_poll->setObjectName(QStringLiteral("psPollTrx"));
    m_poll->setToolTip(QStringLiteral("get_trx_status по таймеру: регистры состояния и температура трансивера "
                                      "AD9361 (v8 п. 3.6.1.16). Только в сервисном режиме."));
    m_pollSeconds = new QSpinBox(this);
    m_pollSeconds->setObjectName(QStringLiteral("psPollTrxSeconds"));
    m_pollSeconds->setRange(kPollMinS, kPollMaxS);
    m_pollSeconds->setValue(kPollDefaultS);
    m_pollSeconds->setSuffix(QStringLiteral(" с"));
    m_pollSeconds->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_pollSeconds->setFixedWidth(56);
    pollRow->addWidget(m_poll);
    pollRow->addWidget(m_pollSeconds);
    pollRow->addStretch(1);
    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &PsDiagnosticsPage::poll);
    connect(m_poll, &QCheckBox::toggled, this, [this](bool on) { setPolling(on, m_pollSeconds->value()); });
    connect(m_pollSeconds, &QSpinBox::valueChanged, this, [this](int s) {
        m_pollTimer->setInterval(s * 1000);
    });

    m_table = new QTableWidget(0, 3, this);
    m_table->setObjectName(QStringLiteral("psDiagnostics"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("Узел"), QStringLiteral("Параметр"), QStringLiteral("Значение")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);

    m_result = new QLabel(QStringLiteral("Значения считываются из микросхем на момент запроса."), this);
    m_result->setObjectName(QStringLiteral("psDiagnosticsResult"));
    m_result->setWordWrap(true);

    layout->addLayout(buttons);
    layout->addLayout(pollRow);
    layout->addWidget(m_table, 1);
    layout->addWidget(m_result);

    connect(m_session->link(), &PsLink::responseReceived, this, &PsDiagnosticsPage::onResponse);
    connect(m_session->link(), &PsLink::commandTimedOut, this, [this](const QString& type, int count) {
        if (m_pending.erase(count) > 0) {
            m_result->setText(QStringLiteral("%1: PS не ответил").arg(type));
        }
    });
    setAllowed(false);
}

void PsDiagnosticsPage::setAllowed(bool allowed)
{
    m_allowed = allowed;
    for (QPushButton* b : m_buttons) {
        b->setEnabled(allowed);
    }
}

void PsDiagnosticsPage::setPolling(bool on, int seconds)
{
    m_pollSeconds->setValue(seconds);
    if (m_poll->isChecked() != on) {
        m_poll->setChecked(on);  // toggled вернётся сюда же
        return;
    }
    m_pollTimer->setInterval(m_pollSeconds->value() * 1000);
    if (on) {
        m_pollTimer->start();
        poll();
    } else {
        m_pollTimer->stop();
    }
}

bool PsDiagnosticsPage::polling() const
{
    return m_pollTimer->isActive();
}

void PsDiagnosticsPage::poll()
{
    // Вне сервисного режима PS get_trx_status не примет — таймер просто ждёт.
    if (!m_allowed) {
        return;
    }
    // Прошлый запрос ещё без ответа — новый не шлём, очередь не копится.
    for (const auto& [count, type] : m_pending) {
        if (type == ps::msg::GetTrxStatus) {
            return;
        }
    }
    request(ps::msg::GetTrxStatus);
}

bool PsDiagnosticsPage::request(std::string_view msgType)
{
    const int count = m_session->send(msgType, json::object());
    if (count < 0) {
        m_result->setText(m_session->lastError());
        return false;
    }
    m_pending[count] = std::string(msgType);
    m_result->setText(QStringLiteral("%1: запрос отправлен").arg(qs(msgType)));
    return true;
}

void PsDiagnosticsPage::onResponse(const ps::Envelope& r, ps::Status status)
{
    const auto it = m_pending.find(r.msg_count);
    if (it == m_pending.end() || it->second != r.msg_type) {
        return;  // запросы других вкладок
    }
    m_pending.erase(it);
    const json& p = r.payload;
    std::vector<Row> rows;
    const auto flags = [&](const std::vector<Flag>& list) {
        for (const Flag& f : list) {
            const auto v = p.find(f.key);
            if (v == p.end() || !v->is_number_integer()) {
                continue;
            }
            const int value = v->get<int>();
            rows.push_back({QString::fromUtf8(f.title),
                            QStringLiteral("%1 (%2)").arg(QString::fromUtf8(value != 0 ? f.on : f.off)).arg(value),
                            value == f.alarmWhen});
        }
    };
    if (r.msg_type == ps::msg::GetTrxStatus) {
        flags(trxFlags());
        if (const auto t = p.find("temp"); t != p.end() && t->is_number()) {
            rows.push_back({QStringLiteral("Температура кристалла"), celsius(t->get<double>()), false});
        }
    } else if (r.msg_type == ps::msg::GetPlStatus) {
        flags(plFlags());
    } else if (r.msg_type == ps::msg::GetRfmTelemetry) {
        if (const auto t = p.find("temp"); t != p.end() && t->is_array()) {
            int n = 1;
            for (const auto& v : *t) {
                if (v.is_number()) {
                    rows.push_back({QStringLiteral("Температура РЧ-модуля %1").arg(n), celsius(v.get<double>()), false});
                }
                ++n;
            }
        }
    }
    const QString name = title(r.msg_type);
    if (status != ps::Status::Ok) {
        // HW_NOT_READY у трансивера приходит с spi_ok = 0 — его тоже показываем.
        m_result->setText(QStringLiteral("%1: PS ответил %2 (%3)")
                              .arg(name, qs(ps::toString(status)), QString::fromStdString(ps::statusRu(status))));
    } else {
        m_result->setText(QStringLiteral("%1: ответ получен").arg(name));
    }
    m_rows[r.msg_type] = std::move(rows);
    render();
}

void PsDiagnosticsPage::render()
{
    int count = 0;
    for (const auto& [type, rows] : m_rows) {
        count += static_cast<int>(rows.size());
    }
    m_table->setRowCount(count);
    int i = 0;
    for (std::string_view type : {ps::msg::GetTrxStatus, ps::msg::GetPlStatus, ps::msg::GetRfmTelemetry}) {
        const auto it = m_rows.find(std::string(type));
        if (it == m_rows.end()) {
            continue;
        }
        for (const Row& row : it->second) {
            m_table->setItem(i, 0, new QTableWidgetItem(title(type)));
            m_table->setItem(i, 1, new QTableWidgetItem(row.name));
            auto* value = new QTableWidgetItem(row.alarm ? QStringLiteral("⚠ ") + row.value : row.value);
            if (row.alarm) {
                value->setForeground(ui::errorColor());
            }
            m_table->setItem(i, 2, value);
            ++i;
        }
    }
}
