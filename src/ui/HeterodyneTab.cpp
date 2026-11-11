#include "HeterodyneTab.h"

#include "PsProtocol.h"
#include "PsSession.h"
#include "UiStyle.h"
#include "heter/HeterFrame.h"
#include "heter/HeterMeasure.h"
#include "heter/HeterSession.h"

#include "heter/HeterRegs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QEventLoop>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QSlider>
#include <QStandardPaths>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace {

class BoundLog final : public QPlainTextEdit {
public:
    explicit BoundLog(QWidget* parent)
        : QPlainTextEdit(parent)
    {
        setReadOnly(true);
        setMaximumBlockCount(500);
    }
    [[nodiscard]] QSize sizeHint() const override { return {260, 140}; }
    [[nodiscard]] QSize minimumSizeHint() const override { return {80, 48}; }
};

QString bits32(quint32 value)
{
    QString bits = QStringLiteral("%1").arg(value, 32, 2, QLatin1Char('0'));
    QString grouped;
    for (int i = 0; i < bits.size(); i += 4) {
        if (i != 0) {
            grouped += QLatin1Char(' ');
        }
        grouped += bits.mid(i, 4);
    }
    return grouped;
}

class HeterPsBridge final : public QObject {
public:
    HeterPsBridge(PsSession* session, QObject* parent)
        : QObject(parent)
        , m_session(session)
    {
    }

    HeterWireReply exchange(const QByteArray& frame, int timeoutMs)
    {
        HeterWireReply got;
        if (m_session == nullptr || !m_session->link()->isReady()) {
            got.error = QStringLiteral("PS: нет связи с платой");
            return got;
        }
        if (!m_session->manualAllowed()) {
            got.error = QStringLiteral("PS: ")
                        + (m_session->lastError().isEmpty() ? QStringLiteral("команды сейчас недоступны")
                                                           : m_session->lastError());
            return got;
        }
        if (!m_session->modeKnown() || m_session->mode() != afar::ps::Mode::Service) {
            got.error = QStringLiteral("PS: нужен сервисный режим");
            return got;
        }
        if (frame.size() >= 3) {
            const int addr = static_cast<quint8>(frame.at(0));
            const auto func = static_cast<quint8>(frame.at(1));
            const int len = static_cast<quint8>(frame.at(2));
            const QByteArray data = frame.mid(3, len);
            if (func == 0x0D) {
                got.error = QStringLiteral("PS: ток накачки эта плата не принимает — в set_lo_params такого поля нет");
                return got;
            }
            if (func == 0x05 || func == 0x06) {
                return loRead(addr, func, timeoutMs);
            }
            if (func == 0x01 || func == 0x02 || func == 0x04) {
                return loWrite(addr, func, data, timeoutMs);
            }
        }
        const QString hex = QString::fromLatin1(frame.toHex()).toUpper();
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        bool answered = false;
        int pending = -1;
        const QMetaObject::Connection response = connect(
            m_session->link(), &PsLink::responseReceived, &loop,
            [&](const afar::ps::Envelope& env, afar::ps::Status status) {
                if (env.msg_count != pending || env.msg_type != afar::ps::msg::Rs485Send) {
                    return;
                }
                answered = true;
                if (status != afar::ps::Status::Ok) {
                    got.error = QStringLiteral("PS: %1")
                                    .arg(QString::fromStdString(afar::ps::statusRu(status)));
                    loop.quit();
                    return;
                }
                const auto data = env.payload.find("data");
                if (data == env.payload.end() || !data->is_string()) {
                    got.error = QStringLiteral("PS: в ответе нет байтов slave");
                    loop.quit();
                    return;
                }
                got.bytes = QByteArray::fromHex(QByteArray::fromStdString(data->get<std::string>()));
                if (got.bytes.isEmpty()) {
                    got.error = QStringLiteral("ответа нет");
                }
                loop.quit();
            });
        const QMetaObject::Connection timedOut = connect(
            m_session->link(), &PsLink::commandTimedOut, &loop, [&](const QString& type, int count) {
                if (count != pending || type != QLatin1String("rs485_send")) {
                    return;
                }
                answered = true;
                got.error = QStringLiteral("PS: нет ответа на rs485_send");
                loop.quit();
            });
        pending = m_session->send(afar::ps::msg::Rs485Send, nlohmann::json{{"data", hex.toStdString()}});
        if (pending < 0) {
            disconnect(response);
            disconnect(timedOut);
            got.error = QStringLiteral("PS: ")
                        + (m_session->lastError().isEmpty() ? QStringLiteral("rs485_send не отправлена")
                                                           : m_session->lastError());
            return got;
        }
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
        disconnect(response);
        disconnect(timedOut);
        if (!answered && got.error.isEmpty()) {
            got.error = QStringLiteral("PS: нет ответа на rs485_send");
        }
        return got;
    }

private:
    HeterWireReply call(std::string_view type, const nlohmann::json& payload, int timeoutMs)
    {
        HeterWireReply got;
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        bool answered = false;
        int pending = -1;
        const QMetaObject::Connection response = connect(
            m_session->link(), &PsLink::responseReceived, &loop,
            [&](const afar::ps::Envelope& env, afar::ps::Status status) {
                if (env.msg_count != pending || env.msg_type != type) {
                    return;
                }
                answered = true;
                if (status != afar::ps::Status::Ok) {
                    got.error = QStringLiteral("PS: %1").arg(QString::fromStdString(afar::ps::statusRu(status)));
                } else {
                    got.bytes = QByteArray::fromStdString(env.payload.dump());
                }
                loop.quit();
            });
        const QMetaObject::Connection timedOut = connect(
            m_session->link(), &PsLink::commandTimedOut, &loop, [&](const QString& gotType, int count) {
                if (count != pending || gotType != QLatin1String(type.data(), static_cast<qsizetype>(type.size()))) {
                    return;
                }
                answered = true;
                got.error = QStringLiteral("PS: нет ответа на %1").arg(QString::fromUtf8(type.data(), static_cast<qsizetype>(type.size())));
                loop.quit();
            });
        pending = m_session->send(type, payload);
        if (pending < 0) {
            disconnect(response);
            disconnect(timedOut);
            got.error = QStringLiteral("PS: ")
                        + (m_session->lastError().isEmpty() ? QString::fromUtf8(type.data(), static_cast<qsizetype>(type.size()))
                                                           : m_session->lastError());
            return got;
        }
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
        disconnect(response);
        disconnect(timedOut);
        if (!answered && got.error.isEmpty()) {
            got.error = QStringLiteral("PS: нет ответа на %1").arg(QString::fromUtf8(type.data(), static_cast<qsizetype>(type.size())));
        }
        return got;
    }

    HeterWireReply loRead(int addr, quint8 func, int timeoutMs)
    {
        const HeterWireReply raw = call(afar::ps::msg::GetLo, afar::ps::loGetPayload(addr), timeoutMs);
        if (!raw.error.isEmpty()) {
            return raw;
        }
        const nlohmann::json payload = nlohmann::json::parse(raw.bytes.constData(), nullptr, false);
        HeterWireReply got;
        if (!payload.is_object()) {
            got.error = QStringLiteral("PS: ответ get_lo_params не разобран");
            return got;
        }
        m_known.insert(addr, payload);
        got.bytes = heter::frame(addr, static_cast<quint8>(func + 0x10), func == 0x06 ? healthWord(payload) : paramsWord(payload));
        return got;
    }

    HeterWireReply loWrite(int addr, quint8 func, const QByteArray& data, int timeoutMs)
    {
        if (!m_known.contains(addr) || !m_known.value(addr).contains("freq")) {
            const HeterWireReply raw = call(afar::ps::msg::GetLo, afar::ps::loGetPayload(addr), timeoutMs);
            if (!raw.error.isEmpty()) {
                return raw;
            }
            const nlohmann::json read = nlohmann::json::parse(raw.bytes.constData(), nullptr, false);
            if (!read.is_object()) {
                HeterWireReply got;
                got.error = QStringLiteral("PS: ответ get_lo_params не разобран");
                return got;
            }
            m_known.insert(addr, read);
        }
        // Плата отвечает INVALID_ARGUMENT, если в set_lo_params пришло одно поле.
        afar::ps::ModuleValues values;
        const nlohmann::json known = m_known.value(addr);
        for (const char* key : {"pwr_b", "pwr_a", "en_b", "en_a", "en_rf", "en_syn", "ref", "freq", "att1", "att2"}) {
            const auto it = known.find(key);
            if (it != known.end() && it->is_number()) {
                values[key] = it->get<double>();
            }
        }
        if (func == 0x02 && data.size() >= 8) {
            values["freq"] = static_cast<double>(heter::readU64(data, 0));
        } else if (func == 0x01 && data.size() >= 2) {
            const quint16 word = heter::readU16(data, 0);
            values["ref"] = word & 1;
            values["en_syn"] = (word >> 1) & 1;
            values["en_rf"] = (word >> 2) & 1;
            values["en_a"] = (word >> 3) & 1;
            values["en_b"] = (word >> 4) & 1;
            values["pwr_a"] = heter::powerDbm((word >> 8) & 3);
            values["pwr_b"] = heter::powerDbm((word >> 10) & 3);
        } else if (func == 0x04 && data.size() >= 2) {
            values["att1"] = (static_cast<quint8>(data.at(0)) & 0x3F) / 2.0;
            values["att2"] = (static_cast<quint8>(data.at(1)) & 0x3F) / 2.0;
        }
        nlohmann::json payload;
        std::string error;
        HeterWireReply got;
        if (values.empty() || !afar::ps::loSetPayload(addr, values, payload, &error)) {
            got.error = QStringLiteral("PS: %1").arg(QString::fromStdString(error.empty() ? "команда не разобрана" : error));
            return got;
        }
        const HeterWireReply raw = call(afar::ps::msg::SetLo, payload, timeoutMs);
        if (!raw.error.isEmpty()) {
            return raw;
        }
        nlohmann::json updated = known;
        for (const auto& [key, value] : values) {
            updated[key] = value;
        }
        m_known.insert(addr, updated);
        got.bytes = heter::frame(addr, static_cast<quint8>(func + 0x10), QByteArray(1, 0));
        return got;
    }

    static double field(const nlohmann::json& payload, const char* key, double fallback)
    {
        const auto it = payload.find(key);
        if (it == payload.end() || !it->is_number()) {
            return fallback;
        }
        return it->get<double>();
    }

    static QByteArray paramsWord(const nlohmann::json& payload)
    {
        const int codeA = heter::powerCode(static_cast<int>(field(payload, "pwr_a", 5)));
        const int codeB = heter::powerCode(static_cast<int>(field(payload, "pwr_b", 5)));
        const quint16 word = static_cast<quint16>(
            (static_cast<int>(field(payload, "ref", 0)) & 1) | ((static_cast<int>(field(payload, "en_syn", 0)) & 1) << 1)
            | ((static_cast<int>(field(payload, "en_rf", 0)) & 1) << 2) | ((static_cast<int>(field(payload, "en_a", 0)) & 1) << 3)
            | ((static_cast<int>(field(payload, "en_b", 0)) & 1) << 4) | ((static_cast<int>(field(payload, "lock", 0)) & 1) << 5)
            | ((codeA < 0 ? 3 : codeA) << 8) | ((codeB < 0 ? 3 : codeB) << 10));
        const quint16 att = static_cast<quint16>((qRound(field(payload, "att1", 31.5) * 2.0) & 0x3F)
                                                 | ((qRound(field(payload, "att2", 31.5) * 2.0) & 0x3F) << 8));
        const bool hasTemp = payload.contains("temp") && payload["temp"].is_number();
        const qint16 temp = hasTemp ? static_cast<qint16>(qBound(-32767, qRound(field(payload, "temp", 0) * 100.0), 32767))
                                    : static_cast<qint16>(kHeterTempNa);
        QByteArray data(14, 0);
        data[0] = static_cast<char>(word & 0xFF);
        data[1] = static_cast<char>((word >> 8) & 0xFF);
        data[2] = static_cast<char>(att & 0xFF);
        data[3] = static_cast<char>((att >> 8) & 0xFF);
        data[4] = static_cast<char>(temp & 0xFF);
        data[5] = static_cast<char>((temp >> 8) & 0xFF);
        const auto freq = static_cast<quint64>(field(payload, "freq", 0));
        for (int i = 0; i < 8; ++i) {
            data[6 + i] = static_cast<char>((freq >> (8 * i)) & 0xFF);
        }
        return data;
    }

    static QByteArray healthWord(const nlohmann::json& payload)
    {
        const bool hasTemp = payload.contains("temp") && payload["temp"].is_number();
        const double temp = field(payload, "temp", 0);
        const quint16 word = static_cast<quint16>((static_cast<int>(field(payload, "lock", 0)) & 1)
                                                 | ((hasTemp && temp < 60.0) ? 2 : 0)
                                                 | ((hasTemp && temp < 80.0) ? 4 : 0));
        return heter::packU16(word);
    }

    PsSession* m_session = nullptr;
    QHash<int, nlohmann::json> m_known;
};

QDoubleSpinBox* mhzBox(double value, QWidget* parent)
{
    auto* box = new QDoubleSpinBox(parent);
    box->setRange(51.5625, 6600);
    box->setDecimals(4);
    box->setSuffix(QStringLiteral(" МГц"));
    box->setValue(value);
    return box;
}

}  // namespace

HeterodyneTab::HeterodyneTab(PsSession* ps, QWidget* parent)
    : QWidget(parent)
    , m_ps(ps)
{
    setObjectName(QStringLiteral("heterodynePage"));
    qRegisterMetaType<HeterState>();
    qRegisterMetaType<HeterReply>();
    qRegisterMetaType<HeterRunStatus>();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(ui::kPageMargin, ui::kPageMargin, ui::kPageMargin, ui::kPageMargin);
    root->setSpacing(ui::kGap);

    auto* row = new QWidget(this);
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(ui::kGap);
    rowLayout->addWidget(buildConnection(), 4);
    rowLayout->addWidget(buildFirmware(), 2);
    m_controls = new QWidget(row);
    auto* controlsLayout = new QHBoxLayout(m_controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(ui::kGap);
    auto* column = new QWidget(m_controls);
    auto* columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(ui::kGap);
    columnLayout->addWidget(buildFrequency(), 1);
    columnLayout->addWidget(buildAttenuators(), 1);
    controlsLayout->addWidget(column, 1);
    controlsLayout->addWidget(buildMode(), 1);
    rowLayout->addWidget(m_controls, 6);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    root->addWidget(row, 0);

    auto* pages = new QTabWidget(this);
    pages->setObjectName(QStringLiteral("heterPages"));
    pages->setDocumentMode(true);
    pages->addTab(buildParameters(), QStringLiteral("Параметры"));
    pages->addTab(buildMeasurePage(), QStringLiteral("Режим измерения"));
    pages->addTab(buildJournal(), QStringLiteral("Журнал"));
    pages->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    root->addWidget(pages, 1);
    setControlsEnabled(false);

    m_thread = new QThread(this);
    m_session = new HeterSession();
    m_measure = new HeterMeasure(m_session);
    auto* bridge = new HeterPsBridge(m_ps, this);
    m_session->setBoardWire(bridge, [bridge](const QByteArray& frame, int timeoutMs) {
        return bridge->exchange(frame, timeoutMs);
    });
    m_serviceTimer = new QTimer(this);
    m_serviceTimer->setSingleShot(true);
    m_session->moveToThread(m_thread);
    m_measure->moveToThread(m_thread);
    connect(m_session, &HeterSession::stateUpdated, this, [this](const HeterState& state) { applyState(state); });
    connect(m_session, &HeterSession::commandDone, this, [this](const QString&, const HeterReply& reply) {
        m_result->setPlainText((reply.tx.isEmpty() ? QString() : QStringLiteral("отправлено: ") + reply.tx + QLatin1Char('\n'))
                               + (reply.rx.isEmpty() ? QString() : QStringLiteral("ответ: ") + reply.rx + QLatin1Char('\n'))
                               + reply.text);
        if (!reply.ok && !reply.text.isEmpty()) {
            m_freqHint->setText(reply.text);
        }
    });
    connect(m_session, &HeterSession::logLine, this, [this](const QString& line) {
        if (!m_showRs->isChecked() && (line.contains(QStringLiteral("RS-485 команда")) || line.contains(QStringLiteral("RS-485 ответ")))) {
            return;
        }
        m_log->appendPlainText(line);
    });
    connect(m_session, &HeterSession::linkFailed, this, [this](const QString& reason) {
        const bool busy = reason.contains(QStringLiteral("busy"), Qt::CaseInsensitive)
                          || reason.contains(QStringLiteral("занят"), Qt::CaseInsensitive);
        m_linkError = busy ? QStringLiteral("порт %1 занят другой программой. Закройте её и нажмите «Подключить» ещё раз.")
                                 .arg(m_port->currentData().toString())
                           : reason;
        m_status->setText(QStringLiteral("нет связи: ") + m_linkError);
        m_connect->setText(QStringLiteral("Подключить"));
        setControlsEnabled(false);
        m_filled = false;
        renderSchema(HeterState{});
    });
    connect(m_session, &HeterSession::flashProgress, this, [this](int percent, const QString& line) {
        m_flashBar->setValue(percent);
        m_flashStatus->setText(line);
    });
    connect(m_session, &HeterSession::flashFinished, this, [this](bool ok, const QString& text) {
        m_flashing = false;
        m_flash->setEnabled(true);
        m_flashBar->setValue(ok ? 100 : m_flashBar->value());
        m_flashStatus->setText(text);
        m_log->appendPlainText(text);
    });
    connect(m_measure, &HeterMeasure::identified, this, [this](const QString& text, bool ok) {
        auto* label = m_identifyKind == QLatin1String("fsw") ? m_fswStatus : m_psuStatus;
        if (label != nullptr) {
            label->setText(text);
        }
        Q_UNUSED(ok);
    });
    connect(m_measure, &HeterMeasure::tuned, this, [this](const QString& text, bool) { m_freqHint->setText(text); });
    connect(m_measure, &HeterMeasure::statusChanged, this, [this](const HeterRunStatus& status) {
        m_runId = status.id;
        m_waitPort = status.port;
        m_runActive = status.status == QLatin1String("running") || status.status == QLatin1String("waiting")
                      || status.status == QLatin1String("finishing");
        m_detail->setText(status.detail);
        m_bar->setMaximum(qMax(1, status.total));
        m_bar->setValue(status.done);
        m_count->setText(QStringLiteral("%1 / %2").arg(status.done).arg(status.total));
        m_last->setText(status.lastLine);
        m_confirm->setVisible(status.status == QLatin1String("waiting"));
        m_confirm->setText(QStringLiteral("Кабель на выходе %1 — продолжить").arg(status.port));
        m_stop->setVisible(status.status == QLatin1String("running") || status.status == QLatin1String("waiting"));
        m_pdfPath = status.pdfPath;
        m_pdf->setVisible(!status.pdfPath.isEmpty());
        m_start->setEnabled(!m_runActive);
        setControlsEnabled(!m_runActive && m_session->state().connected);
        if (!m_runActive && status.status != QLatin1String("idle") && status.status != QLatin1String("running")) {
            m_filled = false;
            refreshArchive();
        }
    });

    m_thread->start();
    QSettings settings;
    const int addr = settings.value(QStringLiteral("heterodyne/addr"), 1).toInt();
    const int addrIndex = m_addr->findData(addr);
    if (addrIndex >= 0) {
        m_addr->setCurrentIndex(addrIndex);
    }
    refreshPorts();
    const QString savedPort = settings.value(QStringLiteral("heterodyne/port")).toString();
    const int portIndex = m_port->findData(savedPort);
    if (portIndex >= 0) {
        m_port->setCurrentIndex(portIndex);
    }
    const QString savedLink = settings.value(QStringLiteral("heterodyne/link"), QStringLiteral("com")).toString();
    const int linkIndex = m_linkKind->findData(savedLink);
    if (linkIndex >= 0) {
        m_linkKind->setCurrentIndex(linkIndex);
    }
    m_port->setEnabled(m_linkKind->currentData().toString() != QLatin1String("ps"));
    if (m_linkKind->currentData().toString() != QLatin1String("ps") && !m_port->currentData().toString().isEmpty()) {
        connectLink();
    }
    connect(m_ps, &PsSession::modeChanged, this, [this] {
        if (m_serviceWait && m_ps->modeKnown() && m_ps->mode() == afar::ps::Mode::Service) {
            m_serviceWait = false;
            m_serviceTimer->stop();
            openHeterodyneOnPs();
        }
    });
    connect(m_ps, &PsSession::modeRequestRefused, this, [this](const QString& reason) { cancelServiceWait(reason); });
    connect(m_serviceTimer, &QTimer::timeout, this, [this] {
        cancelServiceWait(QStringLiteral("плата PS не перешла в сервисный режим"));
    });
}

HeterodyneTab::~HeterodyneTab()
{
    QSettings settings;
    settings.setValue(QStringLiteral("heterodyne/addr"), m_addr->currentData().toInt());
    settings.setValue(QStringLiteral("heterodyne/port"), m_port->currentData().toString());
    settings.setValue(QStringLiteral("heterodyne/link"), m_linkKind->currentData().toString());
    settings.setValue(QStringLiteral("heterodyne/fsw_ip"), m_fswIp->text());
    settings.setValue(QStringLiteral("heterodyne/psu_ip"), m_psuIp->text());
    settings.setValue(QStringLiteral("heterodyne/enables"), m_enables->currentData().toString());
    settings.setValue(QStringLiteral("heterodyne/routing"), m_route->currentData().toString());
    settings.setValue(QStringLiteral("heterodyne/freq_lo"), m_freqLo->value());
    settings.setValue(QStringLiteral("heterodyne/freq_hi"), m_freqHi->value());
    settings.setValue(QStringLiteral("heterodyne/freq_step"), m_freqStep->value());
    settings.setValue(QStringLiteral("heterodyne/att_lo"), m_attLo->value());
    settings.setValue(QStringLiteral("heterodyne/att_hi"), m_attHi->value());
    settings.setValue(QStringLiteral("heterodyne/span"), m_span->value());

    disconnect(m_session, nullptr, this, nullptr);
    disconnect(m_measure, nullptr, this, nullptr);
    m_measure->requestCancel();
    if (m_thread->isRunning()) {
        QMetaObject::invokeMethod(m_session, &HeterSession::closeLink, Qt::BlockingQueuedConnection);
        m_session->deleteLater();
        m_measure->deleteLater();
        m_thread->quit();
        m_thread->wait(4000);
    }
}

QSize HeterodyneTab::sizeHint() const
{
    return {720, 480};
}

QSize HeterodyneTab::minimumSizeHint() const
{
    return {480, 280};
}

QWidget* HeterodyneTab::buildConnection()
{
    auto* box = new QGroupBox(QStringLiteral("Подключение"));
    auto* layout = new QVBoxLayout(box);
    ui::applyGroupLayout(layout);
    m_linkKind = new QComboBox(box);
    m_linkKind->setObjectName(QStringLiteral("heterLinkKind"));
    m_linkKind->addItem(QStringLiteral("COM-порт"), QStringLiteral("com"));
    m_linkKind->addItem(QStringLiteral("Плата PS, RS-485"), QStringLiteral("ps"));
    auto* portRow = new QHBoxLayout();
    m_port = new QComboBox(box);
    m_port->setObjectName(QStringLiteral("heterPort"));
    m_addr = new QComboBox(box);
    m_addr->setObjectName(QStringLiteral("heterAddr"));
    m_addr->addItem(QStringLiteral("01 — смеситель"), 1);
    m_addr->addItem(QStringLiteral("02 — калибровка"), 2);
    auto* refresh = new QPushButton(QStringLiteral("Порты"), box);
    portRow->addWidget(m_port, 1);
    portRow->addWidget(m_addr);
    portRow->addWidget(refresh);
    m_connect = new QPushButton(QStringLiteral("Подключить"), box);
    m_connect->setObjectName(QStringLiteral("heterConnect"));
    ui::setButtonRole(m_connect, ui::ButtonRole::Success);
    m_status = new QLabel(QStringLiteral("нет связи"), box);
    m_status->setObjectName(QStringLiteral("heterStatus"));
    m_status->setWordWrap(true);
    m_clock = new QLabel(QStringLiteral("0,00 с"), box);
    m_clock->setObjectName(QStringLiteral("heterClock"));
    auto* stateRow = new QHBoxLayout();
    stateRow->addWidget(m_status, 1);
    stateRow->addWidget(m_clock);
    layout->addWidget(ui::captionLabel(QStringLiteral("Канал до гетеродина"), box));
    layout->addWidget(m_linkKind);
    layout->addLayout(portRow);
    layout->addWidget(m_connect);
    layout->addLayout(stateRow);

    QSettings settings;
    m_fswIp = new QLineEdit(settings.value(QStringLiteral("heterodyne/fsw_ip")).toString(), box);
    m_psuIp = new QLineEdit(settings.value(QStringLiteral("heterodyne/psu_ip")).toString(), box);
    m_fswIp->setObjectName(QStringLiteral("heterFswIp"));
    m_psuIp->setObjectName(QStringLiteral("heterPsuIp"));
    m_fswIp->setPlaceholderText(QStringLiteral("IP анализатора"));
    m_psuIp->setPlaceholderText(QStringLiteral("IP блока питания"));
    auto* idFsw = new QPushButton(QStringLiteral("Связь с анализатором"), box);
    auto* idPsu = new QPushButton(QStringLiteral("Связь с блоком питания"), box);
    idFsw->setObjectName(QStringLiteral("heterIdentifyFsw"));
    idPsu->setObjectName(QStringLiteral("heterIdentifyPsu"));
    m_fswStatus = ui::hintLabel(QString(), box);
    m_psuStatus = ui::hintLabel(QString(), box);
    m_fswStatus->setObjectName(QStringLiteral("heterFswStatus"));
    m_psuStatus->setObjectName(QStringLiteral("heterPsuStatus"));
    layout->addWidget(ui::captionLabel(QStringLiteral("Анализатор R&S FSW"), box));
    layout->addWidget(m_fswIp);
    layout->addWidget(idFsw);
    layout->addWidget(m_fswStatus);
    layout->addWidget(ui::captionLabel(QStringLiteral("Блок питания"), box));
    layout->addWidget(m_psuIp);
    layout->addWidget(idPsu);
    layout->addWidget(m_psuStatus);
    if (!HeterSession::serialSupported()) {
        m_port->setEnabled(false);
    }
    connect(m_linkKind, &QComboBox::currentIndexChanged, this, [this] {
        const bool viaPs = m_linkKind->currentData().toString() == QLatin1String("ps");
        m_port->setEnabled(!viaPs && HeterSession::serialSupported());
        if (!viaPs || m_connect->text() == QStringLiteral("Отключить")) {
            return;
        }
        if (m_ps == nullptr || !m_ps->link()->isReady()) {
            const QString host = m_ps == nullptr ? QString() : m_ps->host();
            m_status->setText(host.isEmpty()
                                  ? QStringLiteral("нет связи с платой PS. Подключите её на вкладке «Настройки».")
                                  : QStringLiteral("нет связи с платой PS (%1). Подключите её на вкладке «Настройки».")
                                        .arg(host));
            return;
        }
        m_status->setText(QStringLiteral("плата PS на связи (%1). Нажмите «Подключить».").arg(m_ps->host()));
    });
    connect(refresh, &QPushButton::clicked, this, &HeterodyneTab::refreshPorts);
    connect(m_connect, &QPushButton::clicked, this, &HeterodyneTab::connectLink);
    connect(idFsw, &QPushButton::clicked, this, [this] {
        m_identifyKind = QStringLiteral("fsw");
        m_fswStatus->setText(QStringLiteral("проверка…"));
        const QString ip = m_fswIp->text().trimmed();
        QMetaObject::invokeMethod(m_measure, [this, ip] { m_measure->identify(ip, QStringLiteral("analyzer")); },
                                  Qt::QueuedConnection);
    });
    connect(idPsu, &QPushButton::clicked, this, [this] {
        m_identifyKind = QStringLiteral("psu");
        m_psuStatus->setText(QStringLiteral("проверка…"));
        const QString ip = m_psuIp->text().trimmed();
        QMetaObject::invokeMethod(m_measure, [this, ip] { m_measure->identify(ip, QStringLiteral("psu")); },
                                  Qt::QueuedConnection);
    });
    return box;
}

QWidget* HeterodyneTab::buildFirmware()
{
    auto* box = new QGroupBox(QStringLiteral("Прошивка"));
    auto* layout = new QVBoxLayout(box);
    ui::applyGroupLayout(layout);
    m_firmwarePath = new QLineEdit(box);
    m_firmwarePath->setObjectName(QStringLiteral("heterFirmwarePath"));
    m_firmwarePath->setPlaceholderText(QStringLiteral("файл .bin"));
    m_firmwarePath->setReadOnly(true);
    auto* browse = new QPushButton(QStringLiteral("Выбрать файл"), box);
    browse->setObjectName(QStringLiteral("heterFirmwareBrowse"));
    m_firmwareMethod = new QComboBox(box);
    m_firmwareMethod->setObjectName(QStringLiteral("heterFirmwareMethod"));
    m_firmwareMethod->addItem(QStringLiteral("RS-485"), QStringLiteral("rs485"));
    m_firmwareMethod->addItem(QStringLiteral("ST-Link V2"), QStringLiteral("stlink"));
    m_flash = new QPushButton(QStringLiteral("Прошить"), box);
    m_flash->setObjectName(QStringLiteral("heterFirmwareStart"));
    ui::setButtonRole(m_flash, ui::ButtonRole::Primary);
    m_flashBar = new QProgressBar(box);
    m_flashBar->setObjectName(QStringLiteral("heterFirmwareBar"));
    m_flashBar->setRange(0, 100);
    m_flashBar->setValue(0);
    m_flashBar->setTextVisible(false);
    m_flashBar->setFixedHeight(8);
    m_flashStatus = ui::hintLabel(QStringLiteral("Рабочая прошивка — RS-485 или ST-Link. Загрузчик — только ST-Link."), box);
    m_flashStatus->setObjectName(QStringLiteral("heterFirmwareStatus"));
    layout->addWidget(m_firmwarePath);
    layout->addWidget(browse);
    layout->addWidget(m_firmwareMethod);
    layout->addWidget(m_flash);
    layout->addWidget(m_flashBar);
    layout->addWidget(m_flashStatus);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Файл прошивки"), QString(),
                                                          QStringLiteral("Прошивка (*.bin)"));
        if (path.isEmpty()) {
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            m_flashStatus->setText(file.errorString());
            return;
        }
        const HeterFirmwareInfo info = HeterSession::inspectFirmware(file.readAll());
        m_firmwarePath->setText(path);
        m_flashStatus->setText(info.error.isEmpty() ? info.summary : info.error);
        if (info.application) {
            m_firmwareMethod->setCurrentIndex(0);
        } else if (info.error.isEmpty()) {
            m_firmwareMethod->setCurrentIndex(1);
        }
    });
    connect(m_flash, &QPushButton::clicked, this, &HeterodyneTab::startFlash);
    return box;
}

QWidget* HeterodyneTab::buildFrequency()
{
    auto* box = new QGroupBox(QStringLiteral("Частота и ток накачки"));
    auto* layout = new QVBoxLayout(box);
    ui::applyGroupLayout(layout);
    auto* tabs = new QTabWidget(box);
    tabs->setObjectName(QStringLiteral("heterFreqTabs"));
    tabs->setDocumentMode(true);
    tabs->tabBar()->setUsesScrollButtons(false);
    tabs->tabBar()->setExpanding(true);
    tabs->tabBar()->setElideMode(Qt::ElideNone);
    tabs->tabBar()->raise();

    auto* freqPage = new QWidget(tabs);
    auto* freqLayout = new QVBoxLayout(freqPage);
    freqLayout->setContentsMargins(0, ui::kInnerGap, 0, 0);
    m_freq = mhzBox(5070, freqPage);
    m_freq->setObjectName(QStringLiteral("heterFreq"));
    auto* presets = new QHBoxLayout();
    for (double preset : {4170.0, 5070.0, 5270.0, 5370.0}) {
        auto* button = new QPushButton(QString::number(preset, 'f', 0), freqPage);
        button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        presets->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, preset] { applyFrequency(preset); });
    }
    auto* applyFreq = new QPushButton(QStringLiteral("Установить частоту"), freqPage);
    applyFreq->setObjectName(QStringLiteral("heterFreqApply"));
    applyFreq->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    ui::setButtonRole(applyFreq, ui::ButtonRole::Primary);
    m_freqHint = ui::hintLabel(QStringLiteral("Центр анализатора перестраивается вместе с частотой."), freqPage);
    m_freqHint->setObjectName(QStringLiteral("heterFreqHint"));
    freqLayout->addWidget(m_freq);
    freqLayout->addLayout(presets);
    freqLayout->addWidget(applyFreq);
    freqLayout->addWidget(m_freqHint);
    tabs->addTab(freqPage, QStringLiteral("Частота"));
    connect(applyFreq, &QPushButton::clicked, this, [this] { applyFrequency(m_freq->value()); });

    auto* cpPage = new QWidget(tabs);
    auto* cpLayout = new QVBoxLayout(cpPage);
    cpLayout->setContentsMargins(0, ui::kInnerGap, 0, 0);
    m_cp = new QComboBox(cpPage);
    m_cp->setObjectName(QStringLiteral("heterCp"));
    for (int code = 0; code < 16; ++code) {
        m_cp->addItem(QStringLiteral("код %1 · %2 мА").arg(code).arg((code + 1) * 0.3, 0, 'f', 1), code);
    }
    m_cp->setCurrentIndex(2);
    auto* cpApply = new QPushButton(QStringLiteral("Установить ток"), cpPage);
    cpApply->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    cpLayout->addWidget(m_cp);
    cpLayout->addWidget(cpApply);
    cpLayout->addWidget(ui::hintLabel(QStringLiteral("После включения снова ставится код 2, это 0,9 мА."), cpPage));
    tabs->addTab(cpPage, QStringLiteral("Ток накачки"));
    connect(cpApply, &QPushButton::clicked, this, [this] {
        const int code = m_cp->currentData().toInt();
        QMetaObject::invokeMethod(m_session, [this, code] { m_session->setChargePump(code); }, Qt::QueuedConnection);
    });
    connect(m_cp, &QComboBox::currentIndexChanged, this, [this] {
        if (!m_filling && m_schema != nullptr) {
            renderSchema(m_session != nullptr ? m_session->state() : HeterState{});
        }
    });
    layout->addWidget(tabs);
    return box;
}

QWidget* HeterodyneTab::buildAttenuators()
{
    auto* box = new QGroupBox(QStringLiteral("Аттенюаторы"));
    auto* layout = new QVBoxLayout(box);
    ui::applyGroupLayout(layout);
    // Подпись 0 отправляет код 0, подпись −31,5 — код 63.
    const auto attText = [](int slider) {
        const double db = slider / 2.0 - 31.5;
        QString text = QString::number(qAbs(db), 'f', 1).replace(QLatin1Char('.'), QLatin1Char(','));
        if (db < -0.05) {
            text.prepend(QStringLiteral("−"));
        }
        return text + QStringLiteral(" дБ");
    };
    m_att1Value = new QLabel(attText(63), box);
    m_att2Value = new QLabel(attText(63), box);
    m_att1 = new QSlider(Qt::Horizontal, box);
    m_att2 = new QSlider(Qt::Horizontal, box);
    m_att1->setObjectName(QStringLiteral("heterAtt1"));
    m_att2->setObjectName(QStringLiteral("heterAtt2"));
    m_att1->setRange(0, 63);
    m_att2->setRange(0, 63);
    m_att1->setValue(63);
    m_att2->setValue(63);
    const auto sendAtt = [this] {
        if (m_filling) {
            return;
        }
        // Подпись 0 — код 0, подпись −31,5 — код 63.
        const double a = 31.5 - m_att1->value() / 2.0;
        const double b = 31.5 - m_att2->value() / 2.0;
        QMetaObject::invokeMethod(m_session, [this, a, b] { m_session->setAttenuators(a, b); }, Qt::QueuedConnection);
    };
    const auto channel = [this, sendAtt](const QString& caption, QLabel* value, QSlider* slider) {
        auto* row = new QHBoxLayout();
        auto* minus = new QPushButton(QStringLiteral("−0,5"), slider->parentWidget());
        auto* plus = new QPushButton(QStringLiteral("+0,5"), slider->parentWidget());
        minus->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        plus->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        row->addWidget(minus);
        row->addWidget(value, 1);
        row->addWidget(plus);
        auto* block = new QVBoxLayout();
        block->addWidget(ui::captionLabel(caption, slider->parentWidget()));
        block->addLayout(row);
        block->addWidget(slider);
        connect(minus, &QPushButton::clicked, this, [slider, sendAtt] {
            slider->setValue(qMax(0, slider->value() - 1));
            sendAtt();
        });
        connect(plus, &QPushButton::clicked, this, [slider, sendAtt] {
            slider->setValue(qMin(63, slider->value() + 1));
            sendAtt();
        });
        return block;
    };
    layout->addLayout(channel(QStringLiteral("Канал 1"), m_att1Value, m_att1));
    layout->addLayout(channel(QStringLiteral("Канал 2"), m_att2Value, m_att2));
    const auto showAtt = [this, attText] {
        m_att1Value->setText(attText(m_att1->value()));
        m_att2Value->setText(attText(m_att2->value()));
    };
    layout->addWidget(ui::hintLabel(QStringLiteral("0 дБ — максимум, −31,5 дБ — минимум."), box));
    connect(m_att1, &QSlider::valueChanged, this, showAtt);
    connect(m_att2, &QSlider::valueChanged, this, showAtt);
    connect(m_att1, &QSlider::sliderReleased, this, sendAtt);
    connect(m_att2, &QSlider::sliderReleased, this, sendAtt);
    return box;
}

QWidget* HeterodyneTab::buildMode()
{
    auto* mode = new QGroupBox(QStringLiteral("Режим"));
    auto* modeLayout = new QVBoxLayout(mode);
    ui::applyGroupLayout(modeLayout);
    m_ref = new QComboBox(mode);
    m_ref->addItem(QStringLiteral("внутренний 50 МГц"), 0);
    m_ref->addItem(QStringLiteral("внешний"), 1);
    m_pwrA = new QComboBox(mode);
    m_pwrB = new QComboBox(mode);
    for (QComboBox* box : {m_pwrA, m_pwrB}) {
        box->addItem(QStringLiteral("−4 дБм"), -4);
        box->addItem(QStringLiteral("−1 дБм"), -1);
        box->addItem(QStringLiteral("+2 дБм"), 2);
        box->addItem(QStringLiteral("+5 дБм"), 5);
        box->setCurrentIndex(3);
    }
    m_enA = new QCheckBox(QStringLiteral("выход A"), mode);
    m_enB = new QCheckBox(QStringLiteral("выход B"), mode);
    m_enRf = new QCheckBox(QStringLiteral("РЧ-выходы (EN_RF)"), mode);
    m_enSyn = new QCheckBox(QStringLiteral("синтезатор (EN_ADF4355)"), mode);
    m_enA->setChecked(true);
    m_enB->setChecked(true);
    m_enRf->setChecked(true);
    m_enSyn->setChecked(true);
    modeLayout->addWidget(ui::captionLabel(QStringLiteral("Опора"), mode));
    modeLayout->addWidget(m_ref);
    modeLayout->addWidget(ui::captionLabel(QStringLiteral("Мощность A"), mode));
    modeLayout->addWidget(m_pwrA);
    modeLayout->addWidget(ui::captionLabel(QStringLiteral("Мощность B"), mode));
    modeLayout->addWidget(m_pwrB);
    modeLayout->addWidget(m_enA);
    modeLayout->addWidget(m_enB);
    modeLayout->addWidget(m_enRf);
    modeLayout->addWidget(m_enSyn);
    const auto sendCfg = [this] {
        if (m_filling) {
            return;
        }
        const int ref = m_ref->currentData().toInt();
        const int pwrA = m_pwrA->currentData().toInt();
        const int pwrB = m_pwrB->currentData().toInt();
        const bool enA = m_enA->isChecked();
        const bool enB = m_enB->isChecked();
        const bool enRf = m_enRf->isChecked();
        const bool enSyn = m_enSyn->isChecked();
        QMetaObject::invokeMethod(
            m_session, [this, ref, pwrA, pwrB, enA, enB, enRf, enSyn] {
                m_session->setConfig(ref, pwrA, pwrB, enA, enB, enRf, enSyn);
            },
            Qt::QueuedConnection);
    };
    connect(m_ref, &QComboBox::currentIndexChanged, this, sendCfg);
    connect(m_pwrA, &QComboBox::currentIndexChanged, this, sendCfg);
    connect(m_pwrB, &QComboBox::currentIndexChanged, this, sendCfg);
    connect(m_enA, &QCheckBox::toggled, this, sendCfg);
    connect(m_enB, &QCheckBox::toggled, this, sendCfg);
    connect(m_enRf, &QCheckBox::toggled, this, sendCfg);
    connect(m_enSyn, &QCheckBox::toggled, this, sendCfg);
    return mode;
}

QWidget* HeterodyneTab::buildParameters()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kGap);
    m_tempValue = new QLabel(QStringLiteral("нет данных"), page);
    m_tempValue->setObjectName(QStringLiteral("heterTemp"));
    m_tempValue->hide();

    m_boardEmpty = new QLabel(QStringLiteral("Подключите плату по RS-485."), page);
    m_boardEmpty->setWordWrap(true);
    m_boardEmpty->setProperty("role", QStringLiteral("status"));
    ui::setTone(m_boardEmpty, ui::Tone::Warn);

    m_boardLive = new QWidget(page);
    m_boardLive->setObjectName(QStringLiteral("heterSchema"));
    auto* liveLayout = new QVBoxLayout(m_boardLive);
    liveLayout->setContentsMargins(0, 0, 0, 0);
    liveLayout->setSpacing(ui::kInnerGap);

    auto* tiles = new QHBoxLayout();
    tiles->setSpacing(ui::kGap);
    m_tileFreq = new StatTile(QStringLiteral("Частота по RS-485"), m_boardLive);
    m_tileLock = new StatTile(QStringLiteral("Захват ФАПЧ"), m_boardLive);
    m_tileTemp = new StatTile(QStringLiteral("Температура"), m_boardLive);
    m_tileRef = new StatTile(QStringLiteral("Опора"), m_boardLive);
    m_tileSynth = new StatTile(QStringLiteral("Синтезатор"), m_boardLive);
    for (StatTile* tile : {m_tileFreq, m_tileLock, m_tileTemp, m_tileRef, m_tileSynth}) {
        tiles->addWidget(tile, 1);
    }
    liveLayout->addLayout(tiles);

    auto* outputs = new QHBoxLayout();
    outputs->setSpacing(ui::kGap);
    m_outA = new QLabel(m_boardLive);
    m_outB = new QLabel(m_boardLive);
    for (QLabel* out : {m_outA, m_outB}) {
        out->setWordWrap(true);
        out->setProperty("role", QStringLiteral("status"));
        outputs->addWidget(out, 1);
    }
    liveLayout->addLayout(outputs);
    liveLayout->addWidget(ui::hintLabel(
        QStringLiteral("Мощность — код ADF4355-3, не измеренный уровень на разъёме."), m_boardLive));

    m_regIntro = new QLabel(m_boardLive);
    m_regIntro->setWordWrap(true);
    liveLayout->addWidget(m_regIntro);

    auto* regs = new QGridLayout();
    regs->setHorizontalSpacing(ui::kGap);
    regs->setVerticalSpacing(ui::kInnerGap);
    for (int i = 0; i < 13; ++i) {
        auto* card = new QFrame(m_boardLive);
        card->setProperty("role", QStringLiteral("tile"));
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        card->setMinimumHeight(72);
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(10, 8, 10, 8);
        cardLayout->setSpacing(4);
        auto* title = new QLabel(card);
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        title->setWordWrap(true);
        title->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        auto* body = new QLabel(card);
        body->setWordWrap(true);
        body->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cardLayout->addWidget(title);
        cardLayout->addWidget(body);
        const int column = i < 7 ? 0 : 1;
        const int row = i < 7 ? i : i - 7;
        regs->addWidget(card, row, column, Qt::AlignTop);
        m_regCard[i] = card;
        m_regTitle[i] = title;
        m_regBody[i] = body;
    }
    regs->setColumnStretch(0, 1);
    regs->setColumnStretch(1, 1);
    liveLayout->addLayout(regs);

    m_schema = m_boardLive;
    m_boardLive->hide();
    layout->addWidget(m_boardEmpty);
    layout->addWidget(m_boardLive);
    return page;
}

QWidget* HeterodyneTab::buildJournal()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kGap);

    auto* last = new QGroupBox(QStringLiteral("Последняя команда"), page);
    auto* lastLayout = new QVBoxLayout(last);
    ui::applyGroupLayout(lastLayout);
    auto* queryRow = new QHBoxLayout();
    auto* params = new QPushButton(QStringLiteral("Параметры"), last);
    auto* health = new QPushButton(QStringLiteral("Техсостояние"), last);
    params->setObjectName(QStringLiteral("heterParams"));
    health->setObjectName(QStringLiteral("heterHealth"));
    queryRow->addWidget(params);
    queryRow->addWidget(health);
    queryRow->addStretch(1);
    m_result = new QPlainTextEdit(last);
    m_result->setObjectName(QStringLiteral("heterResult"));
    m_result->setReadOnly(true);
    m_result->setPlainText(QStringLiteral("Команд ещё не было."));
    m_result->setMaximumHeight(110);
    lastLayout->addLayout(queryRow);
    lastLayout->addWidget(m_result);
    layout->addWidget(last);

    m_showRs = new QCheckBox(QStringLiteral("Показывать обмен по RS-485"), page);
    m_showRs->setChecked(true);
    m_log = new BoundLog(page);
    m_log->setObjectName(QStringLiteral("heterLog"));
    auto* clearLog = new QPushButton(QStringLiteral("Очистить"), page);
    layout->addWidget(m_showRs);
    layout->addWidget(m_log, 1);
    layout->addWidget(clearLog, 0, Qt::AlignLeft);
    connect(params, &QPushButton::clicked, this, [this] {
        QMetaObject::invokeMethod(m_session, &HeterSession::queryParams, Qt::QueuedConnection);
    });
    connect(health, &QPushButton::clicked, this, [this] {
        QMetaObject::invokeMethod(m_session, &HeterSession::queryHealth, Qt::QueuedConnection);
    });
    connect(clearLog, &QPushButton::clicked, m_log, &QPlainTextEdit::clear);
    return page;
}

QWidget* HeterodyneTab::buildMeasurePage()
{
    auto* box = new QWidget(this);
    box->setObjectName(QStringLiteral("heterMeasure"));
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(ui::kGap);
    m_measureFields = new QWidget(box);
    auto* grid = new QGridLayout(m_measureFields);
    grid->setContentsMargins(0, 0, 0, 0);
    QSettings settings;
    m_enables = new QComboBox(m_measureFields);
    m_enables->addItem(QStringLiteral("Только канал 1 (выход A)"), QStringLiteral("A"));
    m_enables->addItem(QStringLiteral("Только канал 2 (выход B)"), QStringLiteral("B"));
    m_enables->addItem(QStringLiteral("Оба канала"), QStringLiteral("AB"));
    m_route = new QComboBox(m_measureFields);
    m_route->addItem(QStringLiteral("Канал 2, кабель на выходе B"), QStringLiteral("B"));
    m_route->addItem(QStringLiteral("Канал 1, кабель на выходе A"), QStringLiteral("A"));
    m_route->addItem(QStringLiteral("Сначала канал 2, потом канал 1"), QStringLiteral("B-A"));
    m_route->addItem(QStringLiteral("Сначала канал 1, потом канал 2"), QStringLiteral("A-B"));
    m_route->addItem(QStringLiteral("Оба через внешний сумматор"), QStringLiteral("sum"));
    const QString savedEn = settings.value(QStringLiteral("heterodyne/enables"), QStringLiteral("B")).toString();
    const QString savedRoute = settings.value(QStringLiteral("heterodyne/routing"), QStringLiteral("B")).toString();
    if (const int index = m_enables->findData(savedEn); index >= 0) {
        m_enables->setCurrentIndex(index);
    }
    if (const int index = m_route->findData(savedRoute); index >= 0) {
        m_route->setCurrentIndex(index);
    }
    m_freqLo = mhzBox(settings.value(QStringLiteral("heterodyne/freq_lo"), 5070).toDouble(), m_measureFields);
    m_freqHi = mhzBox(settings.value(QStringLiteral("heterodyne/freq_hi"), 5070).toDouble(), m_measureFields);
    m_freqStep = new QDoubleSpinBox(m_measureFields);
    m_freqStep->setRange(0.000001, 6600);
    m_freqStep->setDecimals(4);
    m_freqStep->setSuffix(QStringLiteral(" МГц"));
    m_freqStep->setValue(settings.value(QStringLiteral("heterodyne/freq_step"), 10).toDouble());
    m_attLo = new QDoubleSpinBox(m_measureFields);
    m_attHi = new QDoubleSpinBox(m_measureFields);
    for (QDoubleSpinBox* att : {m_attLo, m_attHi}) {
        att->setRange(0, 31.5);
        att->setSingleStep(0.5);
        att->setDecimals(1);
        att->setSuffix(QStringLiteral(" дБ"));
    }
    m_attLo->setValue(settings.value(QStringLiteral("heterodyne/att_lo"), 0).toDouble());
    m_attHi->setValue(settings.value(QStringLiteral("heterodyne/att_hi"), 31.5).toDouble());
    m_span = new QDoubleSpinBox(m_measureFields);
    m_span->setRange(2, 200);
    m_span->setDecimals(1);
    m_span->setSuffix(QStringLiteral(" МГц"));
    m_span->setValue(settings.value(QStringLiteral("heterodyne/span"), 100).toDouble());
    grid->addWidget(ui::captionLabel(QStringLiteral("Какие каналы включены"), m_measureFields), 0, 0);
    grid->addWidget(m_enables, 1, 0);
    grid->addWidget(ui::captionLabel(QStringLiteral("С какого разъёма снимать"), m_measureFields), 0, 1);
    grid->addWidget(m_route, 1, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("Частота от"), m_measureFields), 2, 0);
    grid->addWidget(m_freqLo, 3, 0);
    grid->addWidget(ui::captionLabel(QStringLiteral("Частота до"), m_measureFields), 2, 1);
    grid->addWidget(m_freqHi, 3, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("Шаг частоты"), m_measureFields), 4, 0);
    grid->addWidget(m_freqStep, 5, 0);
    grid->addWidget(ui::captionLabel(QStringLiteral("Ослабление от"), m_measureFields), 4, 1);
    grid->addWidget(m_attLo, 5, 1);
    grid->addWidget(ui::captionLabel(QStringLiteral("Ослабление до"), m_measureFields), 6, 0);
    grid->addWidget(m_attHi, 7, 0);
    grid->addWidget(ui::captionLabel(QStringLiteral("Полная полоса поиска"), m_measureFields), 6, 1);
    grid->addWidget(m_span, 7, 1);
    layout->addWidget(m_measureFields);
    m_planHint = ui::hintLabel(QString(), box);
    m_planHint->setObjectName(QStringLiteral("heterPlanHint"));
    layout->addWidget(m_planHint);
    auto* buttons = new QHBoxLayout();
    m_start = new QPushButton(QStringLiteral("Начать измерение"), box);
    m_start->setObjectName(QStringLiteral("heterMeasureStart"));
    ui::setButtonRole(m_start, ui::ButtonRole::Primary);
    m_stop = new QPushButton(QStringLiteral("Остановить"), box);
    m_stop->setObjectName(QStringLiteral("heterMeasureStop"));
    ui::setButtonRole(m_stop, ui::ButtonRole::Danger);
    m_stop->setVisible(false);
    m_confirm = new QPushButton(box);
    m_confirm->setObjectName(QStringLiteral("heterConfirmCable"));
    m_confirm->setVisible(false);
    m_pdf = new QPushButton(QStringLiteral("Сохранить PDF"), box);
    m_pdf->setObjectName(QStringLiteral("heterPdf"));
    m_pdf->setVisible(false);
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_confirm);
    buttons->addWidget(m_pdf);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    m_benchMessage = ui::hintLabel(QString(), box);
    m_benchMessage->setObjectName(QStringLiteral("heterBenchMessage"));
    m_detail = new QLabel(box);
    m_detail->setObjectName(QStringLiteral("heterMeasureDetail"));
    m_detail->setWordWrap(true);
    m_bar = new QProgressBar(box);
    m_bar->setObjectName(QStringLiteral("heterMeasureBar"));
    m_count = new QLabel(QStringLiteral("0 / 0"), box);
    m_last = ui::hintLabel(QString(), box);
    m_last->setObjectName(QStringLiteral("heterMeasureLast"));
    layout->addWidget(m_benchMessage);
    layout->addWidget(m_detail);
    layout->addWidget(m_bar);
    layout->addWidget(m_count);
    layout->addWidget(m_last);
    m_archive = new QListWidget(box);
    m_archive->setObjectName(QStringLiteral("heterArchive"));
    m_archive->setMaximumHeight(90);
    layout->addWidget(ui::captionLabel(QStringLiteral("Сохранённые отчёты"), box));
    layout->addWidget(m_archive);

    const auto hint = [this] { updatePlanHint(); };
    connect(m_enables, &QComboBox::currentIndexChanged, this, hint);
    connect(m_route, &QComboBox::currentIndexChanged, this, hint);
    connect(m_freqLo, &QDoubleSpinBox::valueChanged, this, hint);
    connect(m_freqHi, &QDoubleSpinBox::valueChanged, this, hint);
    connect(m_freqStep, &QDoubleSpinBox::valueChanged, this, hint);
    connect(m_attLo, &QDoubleSpinBox::valueChanged, this, hint);
    connect(m_attHi, &QDoubleSpinBox::valueChanged, this, hint);
    connect(m_span, &QDoubleSpinBox::valueChanged, this, hint);
    updatePlanHint();
    refreshArchive();
    connect(m_start, &QPushButton::clicked, this, [this] {
        HeterJob job;
        job.analyzerIp = m_fswIp->text().trimmed();
        job.psuIp = m_psuIp->text().trimmed();
        job.routing = m_route->currentData().toString();
        job.enables = m_enables->currentData().toString();
        job.freqMinMhz = m_freqLo->value();
        job.freqMaxMhz = m_freqHi->value();
        job.freqStepMhz = m_freqStep->value();
        job.attMinDb = m_attLo->value();
        job.attMaxDb = m_attHi->value();
        job.spanMhz = m_span->value();
        const QString error = HeterMeasure::explain(job);
        if (!error.isEmpty()) {
            m_benchMessage->setText(error);
            return;
        }
        if (!m_session->state().connected) {
            m_benchMessage->setText(QStringLiteral("Сначала подключите плату."));
            return;
        }
        const QString dir = reportDirectory();
        m_benchMessage->clear();
        QMetaObject::invokeMethod(m_measure, [this, job, dir] { m_measure->start(job, dir); }, Qt::QueuedConnection);
    });
    connect(m_stop, &QPushButton::clicked, this, [this] { m_measure->requestCancel(); });
    connect(m_confirm, &QPushButton::clicked, this, [this] { m_measure->requestResume(m_runId, m_waitPort); });
    connect(m_pdf, &QPushButton::clicked, this, [this] {
        if (!m_pdfPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_pdfPath));
        }
    });
    connect(m_archive, &QListWidget::itemActivated, this, [](QListWidgetItem* item) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
    });
    return box;
}

void HeterodyneTab::applyFrequency(double mhz)
{
    m_freq->setValue(mhz);
    m_freqHint->setText(QStringLiteral("ставлю частоту на гетеродине…"));
    QMetaObject::invokeMethod(m_session, [this, mhz] { m_session->setFrequencyMhz(mhz); }, Qt::QueuedConnection);
}

void HeterodyneTab::startFlash()
{
    if (m_flashing) {
        return;
    }
    QFile file(m_firmwarePath->text());
    if (!file.open(QIODevice::ReadOnly)) {
        m_flashStatus->setText(QStringLiteral("выберите файл прошивки"));
        return;
    }
    const QByteArray bytes = file.readAll();
    const HeterFirmwareInfo info = HeterSession::inspectFirmware(bytes);
    if (!info.error.isEmpty()) {
        m_flashStatus->setText(info.error);
        return;
    }
    const bool stlink = m_firmwareMethod->currentData().toString() == QLatin1String("stlink");
    if (!stlink && !info.application) {
        m_flashStatus->setText(QStringLiteral("загрузчик пишется только через ST-Link V2"));
        return;
    }
    if (!stlink && (m_session == nullptr || !m_session->state().connected)) {
        m_flashStatus->setText(QStringLiteral("для RS-485 сначала подключите плату"));
        return;
    }
    m_flashing = true;
    m_flash->setEnabled(false);
    m_flashBar->setValue(0);
    m_flashStatus->setText(QStringLiteral("запись…"));
    if (stlink) {
        const QString path = m_firmwarePath->text();
        const quint32 address = info.address;
        QMetaObject::invokeMethod(m_session, [this, path, address] { m_session->flashStlink(path, address); },
                                  Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(m_session, [this, bytes] { m_session->flashRs485(bytes); }, Qt::QueuedConnection);
}

void HeterodyneTab::refreshPorts()
{
    const QString current = m_port->currentData().toString();
    m_port->clear();
    const QList<HeterPortChoice> ports = HeterSession::ports();
    if (ports.isEmpty()) {
        m_port->addItem(QStringLiteral("адаптер USB-RS485 не найден"));
    } else {
        for (const HeterPortChoice& port : ports) {
            m_port->addItem(port.label, port.name);
        }
    }
    const int index = m_port->findData(current);
    if (index >= 0) {
        m_port->setCurrentIndex(index);
    }
}

void HeterodyneTab::cancelServiceWait(const QString& error)
{
    if (!m_serviceWait) {
        return;
    }
    m_serviceWait = false;
    m_serviceTimer->stop();
    m_connect->setText(QStringLiteral("Подключить"));
    m_linkError = error;
    m_status->setText(QStringLiteral("нет связи: ") + error);
}

void HeterodyneTab::openHeterodyneOnPs()
{
    m_status->setText(QStringLiteral("запрос гетеродина по RS-485…"));
    m_connect->setText(QStringLiteral("Отключить"));
    const int addr = m_addr->currentData().toInt();
    QMetaObject::invokeMethod(m_session, [this, addr] { m_session->openViaBoard(addr); }, Qt::QueuedConnection);
}

void HeterodyneTab::connectViaPs()
{
    if (m_ps == nullptr || !m_ps->link()->isReady()) {
        m_linkError = QStringLiteral("сначала подключите плату PS на вкладке «Настройки»");
        m_status->setText(QStringLiteral("нет связи: ") + m_linkError);
        return;
    }
    if (!m_ps->manualAllowed()) {
        m_linkError = m_ps->lastError().isEmpty() ? QStringLiteral("плата PS занята") : m_ps->lastError();
        m_status->setText(QStringLiteral("нет связи: ") + m_linkError);
        return;
    }
    m_userClosed = false;
    m_linkError.clear();
    m_filled = false;
    if (m_ps->modeKnown() && m_ps->mode() == afar::ps::Mode::Service) {
        openHeterodyneOnPs();
        return;
    }
    m_status->setText(QStringLiteral("перевожу плату PS в сервисный режим…"));
    m_connect->setText(QStringLiteral("Отключить"));
    m_serviceWait = true;
    if (m_ps->requestMode(afar::ps::Mode::Service) < 0) {
        cancelServiceWait(m_ps->lastError().isEmpty() ? QStringLiteral("сервисный режим недоступен")
                                                      : m_ps->lastError());
        return;
    }
    m_serviceTimer->start(8000);
}

void HeterodyneTab::connectLink()
{
    if (m_connect->text() == QStringLiteral("Отключить")) {
        m_serviceWait = false;
        m_serviceTimer->stop();
        m_userClosed = true;
        m_linkError.clear();
        QMetaObject::invokeMethod(m_session, &HeterSession::closeLink, Qt::QueuedConnection);
        m_connect->setText(QStringLiteral("Подключить"));
        m_status->setText(QStringLiteral("отключено"));
        m_filled = false;
        setControlsEnabled(false);
        return;
    }
    if (m_linkKind->currentData().toString() == QLatin1String("ps")) {
        connectViaPs();
        return;
    }
    const int addr = m_addr->currentData().toInt();
    const QString port = m_port->currentData().toString().trimmed();
    if (port.isEmpty()) {
        m_linkError = QStringLiteral("адаптер USB-RS485 не найден");
        m_status->setText(QStringLiteral("нет связи: ") + m_linkError);
        renderSchema(HeterState{});
        return;
    }
    m_userClosed = false;
    m_linkError.clear();
    m_filled = false;
    m_status->setText(QStringLiteral("соединение…"));
    m_connect->setText(QStringLiteral("Отключить"));
    QMetaObject::invokeMethod(m_session, [this, port, addr] { m_session->openSerial(port, addr); }, Qt::QueuedConnection);
}

void HeterodyneTab::setControlsEnabled(bool enabled)
{
    m_controls->setEnabled(enabled);
}

void HeterodyneTab::applyState(const HeterState& state)
{
    m_clock->setText(QString::number(state.tMs / 1000.0, 'f', 2) + QStringLiteral(" с"));
    if (!state.connected) {
        if (!m_linkError.isEmpty()) {
            m_status->setText(QStringLiteral("нет связи: ") + m_linkError);
        } else if (m_userClosed) {
            m_status->setText(QStringLiteral("отключено"));
        } else {
            m_status->setText(m_runActive ? QStringLiteral("нет связи · идёт измерение") : QStringLiteral("нет связи"));
        }
        m_tempValue->setText(QStringLiteral("нет данных"));
        renderSchema(state);
        return;
    }
    m_linkError.clear();
    const QString base = QStringLiteral("плата на линии · ") + state.port;
    m_status->setText(m_runActive ? base + QStringLiteral(" · идёт измерение") : base);
    if (!m_filled) {
        m_filling = true;
        m_freq->setValue(state.fOutHz / 1e6);
        m_ref->setCurrentIndex(state.sw2 ? 1 : 0);
        const auto selectPower = [](QComboBox* box, int dbm) {
            const int index = box->findData(dbm);
            if (index >= 0) {
                box->setCurrentIndex(index);
            }
        };
        selectPower(m_pwrA, state.aDbm);
        selectPower(m_pwrB, state.bDbm);
        m_enA->setChecked(state.aEn);
        m_enB->setChecked(state.bEn);
        m_enRf->setChecked(state.enRf);
        m_enSyn->setChecked(state.ce);
        m_att1->setValue(qRound((31.5 - state.attDb[0]) * 2.0));
        m_att2->setValue(qRound((31.5 - state.attDb[1]) * 2.0));
        m_cp->setCurrentIndex(qBound(0, state.cp, 15));
        m_filling = false;
        m_filled = true;
        setControlsEnabled(!m_runActive);
    }
    if (state.tempValid) {
        m_tempValue->setText(QString::number(state.tempC, 'f', 2) + QStringLiteral(" °C"));
    } else {
        m_tempValue->setText(QStringLiteral("нет данных"));
    }
    // Центр анализатора следует за частотой, которую плата реально приняла.
    if (state.fOutHz != 0 && state.fOutHz != m_analyzerHz) {
        m_analyzerHz = state.fOutHz;
        const QString ip = m_fswIp->text().trimmed();
        if (!ip.isEmpty()) {
            const double mhz = state.fOutHz / 1e6;
            m_freqHint->setText(QStringLiteral("перестройка анализатора…"));
            QMetaObject::invokeMethod(m_measure, [this, ip, mhz] { m_measure->tune(ip, mhz); }, Qt::QueuedConnection);
        }
    }
    renderSchema(state);
}

void HeterodyneTab::renderSchema(const HeterState& state)
{
    if (m_boardEmpty == nullptr || m_schema == nullptr) {
        return;
    }
    if (!state.connected) {
        const QString reason = m_linkError.isEmpty() ? QStringLiteral("Подключите плату по RS-485.") : m_linkError;
        m_boardEmpty->setText(QStringLiteral("Нет связи с платой. ") + reason);
        m_boardEmpty->setVisible(true);
        m_schema->setVisible(false);
        return;
    }
    m_boardEmpty->setVisible(false);
    m_schema->setVisible(true);

    const QString temp = state.tempValid ? QString::number(state.tempC, 'f', 2) + QStringLiteral(" °C")
                                         : QStringLiteral("нет данных");
    m_tileFreq->setValue(QString::number(state.fOutHz / 1e6, 'f', 4) + QStringLiteral(" МГц"));
    m_tileFreq->setTone(ui::Tone::Info);
    m_tileLock->setValue(state.lock ? QStringLiteral("LOCK") : QStringLiteral("нет захвата"));
    m_tileLock->setTone(state.lock ? ui::Tone::Ok : ui::Tone::Error);
    m_tileTemp->setValue(temp);
    m_tileTemp->setTone(ui::Tone::Neutral);
    m_tileRef->setValue(state.sw2 ? QStringLiteral("внешняя 50 МГц") : QStringLiteral("50 МГц на плате"));
    m_tileRef->setTone(ui::Tone::Neutral);
    m_tileSynth->setValue(state.ce ? QStringLiteral("включён") : QStringLiteral("выключен"));
    m_tileSynth->setTone(state.ce ? ui::Tone::Ok : ui::Tone::Warn);

    const auto channel = [&](QLabel* label, const QString& name, bool enabled, int power, double att) {
        const bool on = enabled && state.enRf && state.ce;
        const double shown = -att;
        QString attText = QString::number(qAbs(shown), 'f', 1).replace(QLatin1Char('.'), QLatin1Char(','));
        if (shown < -0.05) {
            attText.prepend(QStringLiteral("−"));
        }
        label->setText(QStringLiteral("%1 · %2 · %3 дБм · %4 дБ")
                           .arg(name, on ? QStringLiteral("включён") : QStringLiteral("выключен"))
                           .arg(power)
                           .arg(attText));
        ui::setTone(label, on ? ui::Tone::Ok : ui::Tone::Neutral);
    };
    channel(m_outA, QStringLiteral("Канал 1"), state.aEn, state.aDbm, state.attDb[0]);
    channel(m_outB, QStringLiteral("Канал 2"), state.bEn, state.bDbm, state.attDb[1]);

    const auto hideRegs = [this] {
        for (QFrame* card : m_regCard) {
            card->hide();
        }
    };
    if (state.fOutHz == 0) {
        m_regIntro->setText(QStringLiteral("Частота с платы ещё не прочитана."));
        hideRegs();
        return;
    }
    const int powA = heter::powerCode(state.aDbm);
    const int powB = heter::powerCode(state.bDbm);
    const int cp = m_cp != nullptr ? m_cp->currentData().toInt() : state.cp;
    const HeterRegImage image = heterRegImage(state.fOutHz, cp, state.aEn && state.enRf, powA < 0 ? 3 : powA,
                                              state.bEn && state.enRf, powB < 0 ? 3 : powB);
    if (!image.ok) {
        m_regIntro->setText(image.error);
        hideRegs();
        return;
    }
    m_regIntro->setText(QStringLiteral("Плата регистры по RS-485 не отдаёт. Ниже слова, которые прошивка пишет в микросхему. "
                                      "ГУН %1 МГц, делитель /%2, INT %3, FRAC1 %4, FRAC2 %5, MOD2 %6. "
                                      "Ток накачки %7 мА — последний заданный код, плата его не возвращает.")
                            .arg(image.vcoHz / 1e6, 0, 'f', 3)
                            .arg(image.divider)
                            .arg(image.intN)
                            .arg(image.frac1)
                            .arg(image.frac2)
                            .arg(image.mod2)
                            .arg(image.cpMa, 0, 'f', 1));
    const int count = qMin(image.regs.size(), 13);
    for (int i = 0; i < 13; ++i) {
        const bool visible = i < count;
        m_regCard[i]->setVisible(visible);
        if (!visible) {
            continue;
        }
        const HeterRegView& reg = image.regs.at(i);
        const QString hex = QStringLiteral("%1").arg(reg.word, 8, 16, QLatin1Char('0')).toUpper();
        m_regTitle[i]->setText(reg.title + QStringLiteral("  ·  ") + hex);
        m_regBody[i]->setText(reg.meaning);
        m_regCard[i]->setToolTip(hex + QLatin1Char('\n') + bits32(reg.word));
    }
}

void HeterodyneTab::updatePlanHint()
{
    const QString route = m_route->currentData().toString();
    const int ports = (route == QLatin1String("sum") || route.size() == 1) ? 1 : 2;
    const int steps = 1 + qRound(2.0 * (m_attHi->value() - m_attLo->value()));
    int freqs = 0;
    if (m_freqStep->value() > 0 && m_freqLo->value() <= m_freqHi->value()) {
        double cursor = m_freqLo->value();
        while (cursor < m_freqHi->value() - 1e-9 && freqs <= 256) {
            ++freqs;
            cursor += m_freqStep->value();
        }
        ++freqs;
    }
    const int points = qMax(0, steps) * freqs * ports;
    const QString on = m_enables->currentText();
    const QString where = route == QLatin1String("sum") ? QStringLiteral("Кабель через сумматор на оба выхода.")
                         : route.size() == 1
                             ? QStringLiteral("Кабель на одном выходе.")
                             : QStringLiteral("Программа попросит переставить кабель между выходами.");
    m_planHint->setText(QStringLiteral("Включены %1. %2 Частоты идут от «от» до «до» с заданным шагом, последняя точка всегда «до». "
                                      "Ослабление — от большего к меньшему, шаг 0,5 дБ. Точек и снимков: %3.")
                            .arg(on, where)
                            .arg(points));
}

void HeterodyneTab::refreshArchive()
{
    m_archive->clear();
    QDir dir(reportDirectory());
    const QStringList folders = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    for (const QString& name : folders) {
        const QString pdf = dir.filePath(name + QStringLiteral("/report.pdf"));
        if (!QFile::exists(pdf)) {
            continue;
        }
        auto* item = new QListWidgetItem(name + QStringLiteral(" · PDF"), m_archive);
        item->setData(Qt::UserRole, pdf);
    }
    if (m_archive->count() == 0) {
        m_archive->addItem(QStringLiteral("Готовых отчётов пока нет."));
    }
}

QString HeterodyneTab::reportDirectory() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/heterodyne-reports");
    QDir().mkpath(dir);
    return dir;
}
