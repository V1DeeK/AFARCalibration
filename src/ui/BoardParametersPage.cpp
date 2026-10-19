#include "BoardParametersPage.h"

#include "PsLink.h"
#include "PsProtocol.h"
#include "PsSession.h"
#include "UiStyle.h"

#include <QDateTime>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace ps = afar::ps;
using nlohmann::json;

namespace {

constexpr int kReplyTimeoutMs = 4000;

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QString scalarText(const json& v)
{
    if (v.is_string()) {
        return qs(v.get<std::string>());
    }
    if (v.is_boolean()) {
        return v.get<bool>() ? QStringLiteral("да") : QStringLiteral("нет");
    }
    return qs(v.dump());
}

/// «ключ: значение» по строкам; вложенные объекты — с отступом.
void flatten(const json& j, int depth, QStringList& out)
{
    const QString pad(depth * 2, QLatin1Char(' '));
    if (j.is_object()) {
        for (const auto& [key, value] : j.items()) {
            if (value.is_object() || value.is_array()) {
                out << QStringLiteral("%1%2:").arg(pad, qs(key));
                flatten(value, depth + 1, out);
            } else {
                out << QStringLiteral("%1%2: %3").arg(pad, qs(key), scalarText(value));
            }
        }
    } else if (j.is_array()) {
        for (const auto& value : j) {
            if (value.is_object() || value.is_array()) {
                flatten(value, depth + 1, out);
            } else {
                out << QStringLiteral("%1- %2").arg(pad, scalarText(value));
            }
        }
    } else {
        out << pad + scalarText(j);
    }
}

/// Название запроса для вкладки «Ошибки»: модули — как в «Модулях», чтобы отказ
/// чтения и отказ записи одного модуля были одной строкой.
QString requestTitle(const std::string& type, const json& payload)
{
    if (type == ps::msg::SetLo || type == ps::msg::GetLo) {
        const auto addr = payload.find("addr");
        if (addr != payload.end() && addr->is_number_integer()) {
            return qs(ps::loNameRu(addr->get<int>()));
        }
        return QStringLiteral("Гетеродин");
    }
    if (type == ps::msg::SetMixer || type == ps::msg::GetMixer) {
        return QStringLiteral("Смеситель");
    }
    if (type == ps::msg::SetVideo || type == ps::msg::GetVideo) {
        return QStringLiteral("Видеоприёмник");
    }
    return qs(type);
}

}  // namespace

BoardParametersPage::BoardParametersPage(PsSession* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session)
{
    setObjectName(QStringLiteral("parametersWorkspace"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kInnerGap);

    // Кнопка над статусом: окошко «Тех. состояние» в «Настройках» узкое.
    auto* row = new QVBoxLayout;
    row->setSpacing(ui::kInnerGap);
    m_status = new QLabel(QStringLiteral("Готово к чтению"), this);
    m_status->setObjectName(QStringLiteral("boardParametersStatus"));
    m_status->setWordWrap(true);
    m_read = new QPushButton(QStringLiteral("Получить параметры"), this);
    m_read->setObjectName(QStringLiteral("readBoardParameters"));
    ui::setButtonRole(m_read, ui::ButtonRole::Primary);
    row->addWidget(m_read, 0, Qt::AlignLeft);
    row->addWidget(m_status);

    m_results = new QTabWidget(this);
    m_results->setObjectName(QStringLiteral("boardParametersTabs"));
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_main = new QPlainTextEdit(m_results);
    m_main->setObjectName(QStringLiteral("boardParametersOutput"));
    m_details = new QPlainTextEdit(m_results);
    m_details->setObjectName(QStringLiteral("boardParametersDetails"));
    for (QPlainTextEdit* edit : {m_main, m_details}) {
        edit->setReadOnly(true);
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        edit->setFont(mono);
        edit->setMinimumHeight(120);
    }
    m_main->setPlaceholderText(QStringLiteral("После подключения здесь появятся параметры платы."));
    m_details->setPlaceholderText(QStringLiteral("Здесь появятся ответы PS как они пришли (JSON)."));
    m_errors = new QListWidget(m_results);
    m_errors->setObjectName(QStringLiteral("boardParametersErrors"));
    m_errors->setMinimumHeight(120);
    m_errors->setWordWrap(true);
    m_errors->setSelectionMode(QAbstractItemView::NoSelection);
    m_results->addTab(m_main, QStringLiteral("Основное"));
    m_results->addTab(m_errors, QStringLiteral("Ошибки"));
    m_results->addTab(m_details, QStringLiteral("Подробно"));

    layout->addLayout(row);
    layout->addWidget(m_results, 1);

    m_timeout = new QTimer(this);
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(kReplyTimeoutMs);
    connect(m_timeout, &QTimer::timeout, this, [this] { finish(QStringLiteral("часть ответов не пришла")); });
    connect(m_read, &QPushButton::clicked, this, &BoardParametersPage::requestRead);
    connect(m_session->link(), &PsLink::responseReceived, this,
            [this](const ps::Envelope& env, ps::Status status) {
                const QString key = QStringLiteral("%1#%2").arg(qs(env.msg_type)).arg(env.msg_count);
                if (m_pending.contains(key)) {
                    onResponse(qs(env.msg_type), env.msg_count, static_cast<int>(status), env.payload);
                    return;
                }
                // Ответы на команды других вкладок: отказ — в «Ошибки», успех снимает прежний отказ.
                if (env.msg_type == ps::msg::KeepAlive || env.msg_type == ps::msg::Hello) {
                    return;
                }
                const QString title = requestTitle(env.msg_type, env.payload);
                if (status != ps::Status::Ok) {
                    noteRefusal(title, static_cast<int>(status));
                } else if (m_refusals.remove(title) > 0) {
                    renderErrors();
                }
            });
    // Телеметрия идёт каждую секунду: значения в «Основном» и неисправности в «Ошибках» обновляются сами.
    connect(m_session, &PsSession::telemetryChanged, this, &BoardParametersPage::renderErrors);
    connect(m_session, &PsSession::telemetryChanged, this, &BoardParametersPage::render);
    connect(m_session, &PsSession::modeChanged, this, &BoardParametersPage::render);
    connect(m_session, &PsSession::statusChanged, this, &BoardParametersPage::render);
    renderErrors();
}

void BoardParametersPage::requestRead()
{
    if (!m_session->manualAllowed()) {
        m_status->setText(QStringLiteral("Нет связи с PS — параметры платы запросить нельзя"));
        return;
    }
    m_pending.clear();
    m_replies.clear();
    m_serviceNote = false;
    const auto ask = [this](std::string_view type, const json& payload, const QString& title) {
        const int count = m_session->send(type, payload);
        if (count >= 0) {
            Reply r;
            r.title = title;
            m_pending.insert(QStringLiteral("%1#%2").arg(qs(std::string(type))).arg(count), r);
        }
    };
    ask(ps::msg::GetState, json::object(), QStringLiteral("Состояние изделия"));
    ask(ps::msg::GetOsd, json::object(), QStringLiteral("OSD видеоприёмника"));
    // Состояние трансивера, луч и модули PS отдаёт только в SERVICE.
    if (m_session->mode() == ps::Mode::Service) {
        ask(ps::msg::GetTrxStatus, json::object(), QStringLiteral("Трансивер"));
        ask(ps::msg::GetPlStatus, json::object(), QStringLiteral("ПЛИС"));
        ask(ps::msg::GetRfmTelemetry, json::object(), QStringLiteral("Температуры РЧ-модулей"));
        ask(ps::msg::GetBeam, json::object(), QStringLiteral("Луч"));
        ask(ps::msg::GetLo, ps::loGetPayload(ps::kLoMixer), qs(ps::loNameRu(ps::kLoMixer)));
        ask(ps::msg::GetLo, ps::loGetPayload(ps::kLoCalibration), qs(ps::loNameRu(ps::kLoCalibration)));
        ask(ps::msg::GetMixer, json::object(), QStringLiteral("Смеситель"));
        ask(ps::msg::GetVideo, json::object(), QStringLiteral("Видеоприёмник"));
    } else {
        m_serviceNote = true;
    }
    if (m_pending.isEmpty()) {
        m_status->setText(QStringLiteral("Запрос не ушёл: %1").arg(m_session->lastError()));
        return;
    }
    m_busy = true;
    m_read->setEnabled(false);
    m_status->setText(QStringLiteral("Запрашиваю параметры платы у PS…"));
    emit operationProgress(QStringLiteral("Чтение параметров платы"), 0, static_cast<int>(m_pending.size()));
    m_timeout->start();
    render();
}

void BoardParametersPage::onResponse(const QString& msgType, int msgCount, int status, const json& payload)
{
    const QString key = QStringLiteral("%1#%2").arg(msgType).arg(msgCount);
    const auto it = m_pending.find(key);
    if (it == m_pending.end()) {
        return;  // чужой обмен: кнопки других вкладок, keep_alive
    }
    Reply r = it.value();
    r.status = status;
    r.payload = payload;
    m_pending.erase(it);
    m_replies.push_back(r);
    if (status != static_cast<int>(ps::Status::Ok)) {
        noteRefusal(r.title, status);
    } else if (m_refusals.remove(r.title) > 0) {
        renderErrors();
    }
    emit operationProgress(QStringLiteral("Чтение параметров платы"),
                           static_cast<int>(m_replies.size()),
                           static_cast<int>(m_replies.size() + m_pending.size()));
    render();
    if (m_pending.isEmpty()) {
        finish(QString());
    }
}

void BoardParametersPage::finish(const QString& note)
{
    m_timeout->stop();
    m_pending.clear();
    m_busy = false;
    m_read->setEnabled(true);
    m_status->setText(note.isEmpty() ? QStringLiteral("Параметры платы получены")
                                     : QStringLiteral("Параметры платы получены не полностью: %1").arg(note));
    emit operationProgress(QStringLiteral("Параметры платы получены"), 1, 1);
    render();
}

void BoardParametersPage::render()
{
    QStringList main;
    PsLink* link = m_session->link();
    if (link->isReady()) {
        main << QStringLiteral("Связь: %1:%2, протокол %3")
                    .arg(link->host())
                    .arg(link->port())
                    .arg(link->peerVersion().isEmpty() ? QStringLiteral("—") : link->peerVersion());
    } else {
        main << QStringLiteral("Нет связи с PS");
    }
    if (m_session->modeKnown()) {
        main << QStringLiteral("Режим: %1").arg(qs(ps::modeRu(m_session->mode())));
        if (!m_session->state().isEmpty()) {
            main << QStringLiteral("Состояние: %1").arg(qs(ps::stateRu(m_session->state().toStdString())));
        }
    }
    if (const auto& tlm = m_session->telemetry()) {
        const ps::Telemetry& t = *tlm;
        if (t.uptime > 0) {
            main << QStringLiteral("Время работы: %1 с").arg(t.uptime);
        }
        for (const auto& [id, v] : t.temperature) {
            main << QStringLiteral("Температура %1: %2 °C").arg(qs(id)).arg(v, 0, 'f', 1);
        }
        for (const auto& [id, v] : t.power) {
            main << QStringLiteral("Напряжение %1: %2 В").arg(qs(id)).arg(v, 0, 'f', 3);
        }
        if (t.cpuLoad) {
            QString cores;
            for (double c : t.cpuCores) {
                cores += (cores.isEmpty() ? QString() : QStringLiteral(", ")) + QString::number(c, 'f', 0);
            }
            main << QStringLiteral("Загрузка процессора: %1 %%%2")
                        .arg(*t.cpuLoad, 0, 'f', 1)
                        .arg(cores.isEmpty() ? QString() : QStringLiteral(" (ядра: %1 %)").arg(cores));
        }
        for (const auto& [id, h] : t.devices) {
            main << QStringLiteral("Проверка %1: %2").arg(qs(ps::selfTestRu(id)), qs(std::string(ps::toString(h))));
        }
    }
    if (const int count = m_errors->count(); count > 0 && m_errors->item(0)->data(Qt::UserRole).toBool()) {
        main << QString() << QStringLiteral("Ошибок: %1 — подробно на вкладке «Ошибки»").arg(count);
    }
    if (m_serviceNote) {
        main << QString()
             << QStringLiteral("АЦП, луч, гетеродины, смеситель и видеоприёмник PS отдаёт только в сервисном режиме.");
    }
    QStringList details;
    for (const Reply& r : std::as_const(m_replies)) {
        if (r.status == static_cast<int>(ps::Status::Ok)) {
            // Состояние изделия уже выше, остальное — отдельными разделами.
            if (r.title != QStringLiteral("Состояние изделия")) {
                main << QString() << QStringLiteral("%1").arg(r.title);
                QStringList lines;
                flatten(r.payload, 1, lines);
                main << lines;
            }
        }
        details << QStringLiteral("— %1 (status %2)").arg(r.title, qs(std::string(ps::toString(static_cast<ps::Status>(r.status)))))
                << qs(r.payload.dump(2)) << QString();
    }
    // Телеметрия обновляет страницу раз в секунду: не трогаем текст без перемен и держим прокрутку.
    const auto setText = [](QPlainTextEdit* edit, const QString& text) {
        if (edit->toPlainText() == text) {
            return;
        }
        QScrollBar* bar = edit->verticalScrollBar();
        const int pos = bar->value();
        edit->setPlainText(text);
        bar->setValue(pos);
    };
    setText(m_main, main.join(QLatin1Char('\n')));
    setText(m_details, details.join(QLatin1Char('\n')));
}

void BoardParametersPage::noteRefusal(const QString& title, int status)
{
    m_refusals.insert(title, {QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), status});
    renderErrors();
}

QStringList BoardParametersPage::errors() const
{
    QStringList out;
    for (int i = 0; i < m_errors->count(); ++i) {
        if (m_errors->item(i)->data(Qt::UserRole).toBool()) {
            out << m_errors->item(i)->text();
        }
    }
    return out;
}

void BoardParametersPage::renderErrors()
{
    QStringList lines;
    for (auto it = m_refusals.cbegin(); it != m_refusals.cend(); ++it) {
        lines << QStringLiteral("%1  %2: PS отказал (%3)")
                     .arg(it.value().time, it.key(),
                          qs(std::string(ps::toString(static_cast<ps::Status>(it.value().status)))));
    }
    // Неисправности из UDP-пакета технического состояния — пока они приходят.
    if (const auto& tlm = m_session->telemetry()) {
        for (const auto& [id, h] : tlm->devices) {
            if (h == ps::DeviceHealth::Fail) {
                lines << QStringLiteral("UDP: %1 — неисправность").arg(qs(id));
            }
        }
    }
    if (m_errors->count() > 0 && lines == errors()) {
        return;  // телеметрия раз в секунду: без перемен список не перерисовываем
    }
    m_errors->clear();
    for (const QString& line : std::as_const(lines)) {
        auto* item = new QListWidgetItem(line, m_errors);
        item->setForeground(QBrush(ui::errorColor()));
        item->setData(Qt::UserRole, true);
    }
    if (lines.isEmpty()) {
        auto* item = new QListWidgetItem(QStringLiteral("Ошибок нет"), m_errors);
        item->setForeground(QBrush(ui::okColor()));
    }
    const int index = m_results->indexOf(m_errors);
    m_results->setTabText(index, lines.isEmpty() ? QStringLiteral("Ошибки")
                                                 : QStringLiteral("Ошибки (%1)").arg(lines.size()));
    m_results->tabBar()->setTabTextColor(index, lines.isEmpty() ? QColor() : ui::errorColor());
    render();
}
