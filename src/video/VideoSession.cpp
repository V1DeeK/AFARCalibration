#include "VideoSession.h"

#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <optional>

#ifdef AFAR_HAVE_SERIALPORT
#include <QSerialPort>
#endif

using video::Status;

namespace {

/// Отказ обмена. code: −1 — логика узла, 0 — нет ответа, >0 — исключение Modbus.
struct VideoError {
    QString message;
    int code = -1;
};

struct StepResult {
    QString result = QStringLiteral("PASS");
    QString text;
};

[[noreturn]] void fail(const QString& message)
{
    throw VideoError{message, -1};
}

QString fmtMhz(double hz)
{
    return QStringLiteral("%1 МГц").arg(hz / 1e6, 0, 'f', 3);
}

const QList<QPair<QString, QList<int>>>& channelPlan()
{
    static const QList<QPair<QString, QList<int>>> plan{
        {QStringLiteral("R"), {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917}},
        {QStringLiteral("A"), {5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725}},
        {QStringLiteral("B"), {5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866}},
        {QStringLiteral("E"), {5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945}},
        {QStringLiteral("F"), {5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880}},
    };
    return plan;
}

}  // namespace

VideoSession::VideoSession(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<video::Status>();
    qRegisterMetaType<video::ApplyRequest>();
    qRegisterMetaType<video::TestOptions>();
    qRegisterMetaType<QList<quint16>>();
    qRegisterMetaType<QList<quint32>>();
    m_poll = new QTimer(this);
    m_poll->setInterval(300);
    connect(m_poll, &QTimer::timeout, this, &VideoSession::poll);
}

VideoSession::~VideoSession()
{
    closePort();
}

bool VideoSession::serialSupported()
{
#ifdef AFAR_HAVE_SERIALPORT
    return true;
#else
    return false;
#endif
}

void VideoSession::openPort(const QString& portName, int addr)
{
    closePort();
#ifdef AFAR_HAVE_SERIALPORT
    m_addr = static_cast<quint8>(std::clamp(addr, 1, 247));
    m_port = new QSerialPort(this);
    m_port->setPortName(portName);
    m_port->setBaudRate(QSerialPort::Baud115200);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::EvenParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);
    if (!m_port->open(QIODevice::ReadWrite)) {
        const QString reason = m_port->errorString();
        delete m_port;
        m_port = nullptr;
        qWarning("VideoSession: порт %s не открыт: %s", qPrintable(portName), qPrintable(reason));
        emit logLine(QStringLiteral("er"), QStringLiteral("Не удалось открыть порт %1: %2").arg(portName, reason));
        emit linkChanged(QStringLiteral("err"), QStringLiteral("ошибка порта"));
        return;
    }
    m_ok = 0;
    m_errors = 0;
    m_pollFails = 0;
    m_lastTx.start();
    emit logLine(QStringLiteral("in"), QStringLiteral("Порт %1 открыт: 115200 8E1, адрес %2").arg(portName).arg(m_addr));
    emit linkChanged(QStringLiteral("on"), QStringLiteral("порт открыт"));
    m_poll->start();
#else
    Q_UNUSED(portName);
    Q_UNUSED(addr);
    emit linkChanged(QStringLiteral("err"), QStringLiteral("сборка без Qt SerialPort"));
#endif
}

void VideoSession::closePort()
{
    m_poll->stop();
#ifdef AFAR_HAVE_SERIALPORT
    if (m_port != nullptr) {
        m_port->close();
        delete m_port;
        m_port = nullptr;
        emit linkChanged(QStringLiteral("off"), QStringLiteral("не подключено"));
    }
#endif
}

QList<quint16> VideoSession::exchange(const QByteArray& frame, const Options& opt)
{
#ifdef AFAR_HAVE_SERIALPORT
    if (m_port == nullptr) {
        throw VideoError{QStringLiteral("порт не открыт"), 0};
    }
    const bool show = !opt.quiet || m_logPoll.load();
    for (int attempt = 0; attempt <= opt.retries; ++attempt) {
        // Пауза между кадрами не меньше t3,5 (1,75 мс на 115200).
        if (m_lastTx.isValid() && m_lastTx.elapsed() < 3) {
            QThread::msleep(3);
        }
        m_port->clear(QSerialPort::Input);
        if (show) {
            emit logLine(QStringLiteral("tx"), QStringLiteral("→ ") + video::hexBytes(frame));
        }
        QByteArray rx;
        if (m_port->write(frame) == frame.size() && m_port->waitForBytesWritten(200)) {
            QElapsedTimer total;
            total.start();
            QElapsedTimer quiet;
            while (total.elapsed() < opt.timeoutMs) {
                if (m_port->waitForReadyRead(5) || m_port->bytesAvailable() > 0) {
                    rx += m_port->readAll();
                    quiet.start();
                }
                const int need = video::expectedLength(rx);
                if (need > 0 && rx.size() >= need) {
                    rx.truncate(need);
                    break;
                }
                // Кадр неизвестной длины кончается тишиной.
                if (!rx.isEmpty() && quiet.isValid() && quiet.elapsed() > 15) {
                    break;
                }
            }
        }
        m_lastTx.start();
        const video::Reply reply = video::parseReply(frame, rx);
        if (show && !rx.isEmpty()) {
            emit logLine(QStringLiteral("rx"), QStringLiteral("← ") + video::hexBytes(rx));
        }
        switch (reply.kind) {
        case video::ReplyKind::Ok:
            ++m_ok;
            return reply.words;
        case video::ReplyKind::Exception:
            ++m_ok;
            throw VideoError{QStringLiteral("исключение %1: %2").arg(reply.exception).arg(video::exceptionText(reply.exception)),
                             reply.exception};
        case video::ReplyKind::NoReply:
            if (show) {
                emit logLine(QStringLiteral("er"), QStringLiteral("  нет ответа"));
            }
            break;
        case video::ReplyKind::BadFrame:
            emit logLine(QStringLiteral("er"), QStringLiteral("  ошибка CRC/длины"));
            break;
        case video::ReplyKind::WrongAddress:
            emit logLine(QStringLiteral("er"), QStringLiteral("  ответ с чужого адреса"));
            break;
        case video::ReplyKind::WrongFunction:
            emit logLine(QStringLiteral("er"), QStringLiteral("  неожиданная функция"));
            break;
        }
    }
    ++m_errors;
    throw VideoError{QStringLiteral("нет ответа"), 0};
#else
    Q_UNUSED(frame);
    Q_UNUSED(opt);
    throw VideoError{QStringLiteral("сборка без Qt SerialPort"), 0};
#endif
}

QList<quint16> VideoSession::input(quint16 reg, quint16 count, const Options& opt)
{
    const QList<quint16> words = exchange(video::readFrame(m_addr, 4, reg, count), opt);
    if (words.size() != count) {
        throw VideoError{QStringLiteral("в ответе %1 регистров вместо %2").arg(words.size()).arg(count), -1};
    }
    return words;
}

QList<quint16> VideoSession::input(quint16 reg, quint16 count)
{
    return input(reg, count, Options{});
}

void VideoSession::writeMulti(quint16 reg, const QList<quint16>& values, const Options& opt)
{
    exchange(video::writeMultiFrame(m_addr, reg, values), opt);
}

void VideoSession::writeMulti(quint16 reg, const QList<quint16>& values)
{
    writeMulti(reg, values, Options{});
}

Status VideoSession::readStatus(bool quiet)
{
    const Options q{150, 1, quiet};
    const Options q0{150, 0, quiet};
    const QList<quint16> r = input(0, 0x14, q);
    Status s;
    s.mapVersion = r[0];
    s.moduleType = r[1];
    s.fwMajor = r[2];
    s.fwMinor = r[3];
    s.bootId = video::u32(r, 4);
    s.appliedSeq = video::u32(r, 6);
    s.state = r[8];
    s.lastError = r[9];
    s.flags = video::u32(r, 10);
    s.epoch = video::u32(r, 12);
    s.uptimeMs = video::u32(r, 14);
    s.acceptedSeq = video::u32(r, 16);
    s.caps = video::u32(r, 18);
    try {
        const QList<quint16> f = input(0x0F00, 2, q0);
        s.fwMode = f[0];
        s.blVersion = f[1];
    } catch (const VideoError&) {
        s.fwMode = 1;  // прошивка без VIDEO-FWUP
    }
    if (s.fwMode == 1) {
        try {
            const QList<quint16> x = input(0x0400, 7, q0);
            s.agcMode = x[0];
            s.manualAtten = x[1];
            s.targetMv = x[2];
            s.rssiMv = x[3];
            s.attenCode = x[4];
            s.band = x[5];
            s.osdStat = x[6];
            try {
                const QList<quint16> p = input(0x0407, 3, q0);
                s.rtcPaths = p[0];
                s.rtcPd = video::u32(p, 1);
            } catch (const VideoError&) {  // прошивка без RTC_PATHS
            }
            try {
                s.rtcRead = video::u32(input(0x040A, 2, q0), 0);
                s.hasRtcRead = true;
            } catch (const VideoError&) {  // прошивка без чтения RTC6715
            }
            try {
                const QList<quint16> b = input(0x040C, 2, q0);
                s.bandMode = b[0];
                s.cpCode = b[1];
            } catch (const VideoError&) {  // прошивка без выбора фильтра
            }
            const QList<quint16> y = input(0x0100, 8, q0);
            s.freqHz = video::u64(y, 0);
            s.rfEnable = y[4];
            s.power = y[5];
            s.profile = y[6];
            s.calcHz = video::u64(input(0x0110, 4, q0), 0);
            s.hasExt = true;
        } catch (const VideoError&) {  // старая прошивка без VIDEO-EXT
        }
    } else {
        const QList<quint16> f = input(0x0F02, 13, q);
        s.fwupState = f[0];
        s.fwupError = f[1];
        s.fwupMax = video::u32(f, 2);
        s.fwupWritten = video::u32(f, 4);
        s.appValid = f[6];
        s.appSize = video::u32(f, 7);
        s.appCrc = video::u32(f, 9);
        s.appVersion = video::u32(f, 11);
    }
    emit statusUpdated(s);
    emit statsChanged(m_ok, m_errors);
    return s;
}

void VideoSession::pause(int ms)
{
    QThread::msleep(static_cast<unsigned long>(std::max(0, ms)));
}

Status VideoSession::transaction(quint16 cmd, const QList<Group>& groups, int timeoutMs)
{
    const Status st = readStatus();
    if (st.fwMode == 2) {
        fail(QStringLiteral("узел в загрузчике"));
    }
    m_seq = std::max({m_seq, st.acceptedSeq, st.appliedSeq}) + 1;
    const quint32 seq = m_seq;
    QList<quint16> header = video::w32(seq);
    header += video::w32(st.epoch);
    header += {cmd, 0};
    header += video::w32(static_cast<quint32>(timeoutMs));
    writeMulti(0x0000, header);
    for (const Group& g : groups) {
        writeMulti(g.first, g.second);
    }
    try {
        exchange(video::writeSingleFrame(m_addr, 0x0008, video::kCommitMagic), Options{});
    } catch (const VideoError& e) {
        const Status s2 = readStatus();
        fail(QStringLiteral("COMMIT отклонён: %1, LAST_ERROR=%2").arg(e.message, video::errorName(s2.lastError)));
    }
    QElapsedTimer t;
    t.start();
    for (;;) {
        pause(20);
        const Status s = readStatus();
        if (s.appliedSeq == seq && (s.state == 4 || s.state == 6) && s.lastError == 0) {
            return s;
        }
        if (s.state == 5) {
            fail(QStringLiteral("FAULT: ") + video::errorName(s.lastError));
        }
        if (t.elapsed() > timeoutMs + 1500) {
            fail(QStringLiteral("нет подтверждения: STATE=%1, LAST_ERROR=%2")
                     .arg(video::stateName(s.state), video::errorName(s.lastError)));
        }
    }
}

Status VideoSession::applySynth(const video::ApplyRequest& a)
{
    QList<quint16> synth = video::w64(static_cast<quint64>(std::llround(a.freqHz)));
    synth += {static_cast<quint16>(a.rf ? 1 : 0), static_cast<quint16>(a.power), static_cast<quint16>(a.profile), 0};
    QList<Group> groups{{0x0100, synth}};
    if (a.sendExt) {
        groups.append({0x0400, {static_cast<quint16>(a.agcMode), static_cast<quint16>(a.attenCode),
                                static_cast<quint16>(a.targetMv), static_cast<quint16>(a.bandMode)}});
    }
    return transaction(video::CmdApply, groups);
}

Status VideoSession::settled(int ms)
{
    // RSSI на плате устанавливается 2–3 с; опрос в ожидании держит связь, без него узел уйдёт в SAFE.
    QElapsedTimer t;
    t.start();
    Status s;
    do {
        pause(250);
        s = readStatus();
    } while (t.elapsed() < ms && !m_abort.load());
    return s;
}

void VideoSession::runOp(const QString& op, const std::function<QString()>& body)
{
    try {
        const QString text = body();
        emit opDone(op, true, text.isEmpty() ? QStringLiteral("выполнено") : text);
    } catch (const VideoError& e) {
        emit logLine(QStringLiteral("er"), e.message);
        emit opDone(op, false, e.message);
    }
}

void VideoSession::poll()
{
    if (m_port == nullptr || !m_pollEnabled.load()) {
        return;
    }
    try {
        const Status s = readStatus();
        m_pollFails = 0;
        emit linkChanged(s.fwMode == 2 ? QStringLiteral("boot") : QStringLiteral("on"),
                         s.fwMode == 2 ? QStringLiteral("загрузчик") : QStringLiteral("на связи"));
    } catch (const VideoError&) {
        if (++m_pollFails >= 3) {
            emit linkChanged(QStringLiteral("err"), QStringLiteral("нет ответа"));
        }
    }
}

void VideoSession::apply(const video::ApplyRequest& request)
{
    runOp(QStringLiteral("apply"), [&] {
        const Status s = applySynth(request);
        return QStringLiteral("APPLIED, ")
               + (s.flags & video::FlagPllLock ? QStringLiteral("PLL захвачен") : QStringLiteral("нет флага захвата"));
    });
    readRegisters();
}

void VideoSession::safe()
{
    runOp(QStringLiteral("apply"), [&] {
        transaction(video::CmdSafe, {});
        return QStringLiteral("SAFE");
    });
}

void VideoSession::clearFault()
{
    runOp(QStringLiteral("apply"), [&] {
        transaction(video::CmdClearFault, {});
        return QStringLiteral("FAULT сброшен, узел в SAFE");
    });
}

void VideoSession::writeChargePump(int code)
{
    runOp(QStringLiteral("cp"), [&] {
        writeMulti(0x0411, {static_cast<quint16>(code)});
        const QList<quint16> r = input(0x040D, 1);
        if (r[0] != code) {
            fail(QStringLiteral("прочитано %1").arg(r[0]));
        }
        return QStringLiteral("код %1 записан (R4, R6)").arg(code);
    });
    readRegisters();
}

void VideoSession::writeRtcPaths(int value)
{
    runOp(QStringLiteral("rtc"), [&] {
        writeMulti(0x0410, {static_cast<quint16>(value)});
        const QList<quint16> p = input(0x0407, 3);
        if (p[0] != value) {
            fail(QStringLiteral("RTC_PATHS прочитано %1, записано %2").arg(p[0]).arg(value));
        }
        return QStringLiteral("аудио %1, ВЧ %2, рег. 0x0A = 0x%3")
            .arg(value & 1 ? QStringLiteral("вкл") : QStringLiteral("выкл"),
                 value & 2 ? QStringLiteral("вкл") : QStringLiteral("выкл"),
                 QString::number(video::u32(p, 1), 16).toUpper());
    });
}

void VideoSession::readRegisters()
{
    runOp(QStringLiteral("regs"), [&] {
        const QList<quint16> r = input(0x0420, 26);
        QList<quint32> words;
        for (int i = 0; i < 13; ++i) {
            words.append(video::u32(r, 2 * i));
        }
        emit registersRead(words);
        return QStringLiteral("прочитано");
    });
}

void VideoSession::osdWrite(int standard, bool enable, int row, int col, const QList<quint16>& glyphs, int attr)
{
    runOp(QStringLiteral("osd"), [&] {
        if (glyphs.isEmpty()) {
            fail(QStringLiteral("пустой текст"));
        }
        QList<quint16> v{static_cast<quint16>(standard), static_cast<quint16>(enable ? 1 : 0),
                         static_cast<quint16>(row), static_cast<quint16>(col),
                         static_cast<quint16>(glyphs.size()), static_cast<quint16>(attr), 1, 0};
        v += glyphs;
        transaction(video::CmdOsdWrite, {{0x0300, v}}, 200);
        return QStringLiteral("записано %1 симв.").arg(glyphs.size());
    });
}

void VideoSession::runTest(const video::TestOptions& opt)
{
    m_abort.store(false);
    int fails = 0;
    int warns = 0;
    bool stopped = false;
    const auto step = [&](const QString& name, const std::function<StepResult()>& body) {
        StepResult r;
        if (stopped || m_abort.load()) {
            stopped = true;
            r = {QStringLiteral("SKIP"), QStringLiteral("остановлено")};
        } else {
            try {
                r = body();
            } catch (const VideoError& e) {
                r = {QStringLiteral("FAIL"), e.message};
            }
        }
        fails += r.result == QLatin1String("FAIL");
        warns += r.result == QLatin1String("WARN");
        emit testStep(name, r.result, r.text);
    };
    const auto ext = [](int agc, int atten, int target) {
        video::ApplyRequest a;
        a.agcMode = agc;
        a.attenCode = atten;
        a.targetMv = target;
        return a;
    };

    std::optional<Status> id;
    step(QStringLiteral("Связь, идентификация узла"), [&]() -> StepResult {
        const Status st = readStatus(false);
        id = st;
        if (st.mapVersion != 1) {
            fail(QStringLiteral("MAP_VERSION=%1").arg(st.mapVersion));
        }
        if (st.moduleType != 2) {
            fail(QStringLiteral("MODULE_TYPE=%1 (ожидается 2 VIDEO)").arg(st.moduleType));
        }
        if (st.fwMode == 2) {
            fail(QStringLiteral("узел в загрузчике — сначала прошейте приложение"));
        }
        return {QStringLiteral("PASS"),
                QStringLiteral("ПО %1, загрузчик %2, BOOT_ID %3").arg(st.fwText(), st.blText(), st.bootText())};
    });
    if (!id || id->fwMode == 2 || id->mapVersion != 1 || id->moduleType != 2) {
        stopped = true;
    }
    const Status st = id.value_or(Status{});

    step(QStringLiteral("Загрузчик RS-485 установлен"), [&]() -> StepResult {
        return st.blVersion ? StepResult{QStringLiteral("PASS"), QStringLiteral("версия ") + st.blText()}
                            : StepResult{QStringLiteral("WARN"), QStringLiteral("нет — обновление по RS-485 недоступно")};
    });
    step(QStringLiteral("OSD AT7456E инициализирован"), [&]() -> StepResult {
        if (!(st.flags & video::FlagOsdReady)) {
            fail(QStringLiteral("OSD_READY = 0"));
        }
        return {QStringLiteral("PASS"), QStringLiteral("OSD_READY")};
    });

    for (const int mhz : {4900, 5200, 5499, 5500, 5800, 6000}) {
        step(QStringLiteral("Синтезатор: %1 МГц (LO %2)").arg(mhz).arg(mhz - 480), [&]() -> StepResult {
            video::ApplyRequest a = ext(1, 32, 850);
            a.freqHz = mhz * 1e6;
            const Status s = applySynth(a);
            if (!(s.flags & video::FlagPllLock)) {
                fail(QStringLiteral("нет захвата PLL (MUXOUT)"));
            }
            const Status s2 = readStatus();
            const double calc = static_cast<double>(s2.calcHz);
            if (std::abs(calc - mhz * 1e6) > 1) {
                fail(QStringLiteral("CALCULATED_HZ = %1").arg(s2.calcHz));
            }
            const int band = mhz < 5500 ? 0 : 1;
            if (s2.band != band) {
                fail(QStringLiteral("поддиапазон %1, ожидался %2").arg(s2.band).arg(band));
            }
            return {QStringLiteral("PASS"),
                    QStringLiteral("захват, расчёт %1, поддиапазон %2")
                        .arg(fmtMhz(calc), band ? QStringLiteral("5,5–6,0") : QStringLiteral("4,9–5,5"))};
        });
    }

    int rssiLo = -1;
    int rssiHi = -1;
    const double sigHz = (opt.signalMhz > 0 ? opt.signalMhz : 5800.0) * 1e6;
    step(QStringLiteral("Аттенюатор HMC624A: 0 и 31,5 дБ"), [&]() -> StepResult {
        video::ApplyRequest a = ext(1, 0, 850);
        a.freqHz = sigHz;
        applySynth(a);
        Status s = settled(3000);
        if (s.attenCode != 0) {
            fail(QStringLiteral("ATTEN_CODE=%1").arg(s.attenCode));
        }
        const int lo = s.rssiMv;
        a.attenCode = 63;
        applySynth(a);
        s = settled(3000);
        if (s.attenCode != 63) {
            fail(QStringLiteral("ATTEN_CODE=%1").arg(s.attenCode));
        }
        rssiLo = lo;
        rssiHi = s.rssiMv;
        return {QStringLiteral("PASS"), QStringLiteral("RSSI: 0 дБ → %1 мВ, 31,5 дБ → %2 мВ").arg(rssiLo).arg(rssiHi)};
    });
    step(QStringLiteral("RSSI в допустимом диапазоне АЦП"), [&]() -> StepResult {
        if (rssiLo < 0) {
            return {QStringLiteral("SKIP"), {}};
        }
        // Брак — только упор в края АЦП. Норма даташита RTC6715 0,5–1,1 В — предупреждение:
        // на плате №1 шум ≈ 1,9 В, сигнал до 3,1 В.
        const int lo = std::min(rssiLo, rssiHi);
        const int hi = std::max(rssiLo, rssiHi);
        if (lo < 100 || hi > 3250) {
            fail(QStringLiteral("%1 / %2 мВ — упор в край АЦП (0,1…3,25 В)").arg(rssiLo).arg(rssiHi));
        }
        if (lo < 500 || hi > 1100) {
            return {QStringLiteral("WARN"),
                    QStringLiteral("%1 / %2 мВ — АЦП в норме, но вне 0,5–1,1 В по даташиту RTC6715").arg(rssiLo).arg(rssiHi)};
        }
        return {QStringLiteral("PASS"), QStringLiteral("в пределах 0,5–1,1 В")};
    });
    step(QStringLiteral("Реакция RSSI на ослабление"), [&]() -> StepResult {
        if (!opt.haveSignal) {
            return {QStringLiteral("SKIP"), QStringLiteral("тестовый сигнал не подан")};
        }
        if (rssiLo < 0) {
            return {QStringLiteral("SKIP"), {}};
        }
        const int d = rssiLo - rssiHi;
        if (d < 100) {
            fail(QStringLiteral("разница %1 мВ < 100 мВ — тракт или аттенюатор неисправны").arg(d));
        }
        return {QStringLiteral("PASS"), QStringLiteral("разница %1 мВ").arg(d)};
    });
    step(QStringLiteral("АРУ выходит в рабочую зону"), [&]() -> StepResult {
        if (!opt.haveSignal) {
            return {QStringLiteral("SKIP"), QStringLiteral("тестовый сигнал не подан")};
        }
        // Уставка — середина между RSSI при 0 и 31,5 дБ: достижима на любой шкале RSSI.
        const int tgt = rssiLo >= 0 && rssiLo - rssiHi >= 100
                            ? static_cast<int>(std::lround((rssiLo + rssiHi) / 20.0)) * 10 : 850;
        video::ApplyRequest a = ext(0, 0, tgt);
        a.freqHz = sigHz;
        applySynth(a);
        QElapsedTimer t;
        t.start();
        Status s;
        do {
            pause(250);
            s = readStatus();
        } while (!m_abort.load() && t.elapsed() < 40000 && !(std::abs(s.rssiMv - tgt) <= 60 && t.elapsed() > 4000));
        const QString where = QStringLiteral("уставка %1 мВ: RSSI %2 мВ, ослабление %3 дБ")
                                  .arg(tgt).arg(s.rssiMv).arg(s.attenCode / 2.0, 0, 'f', 1);
        if (std::abs(s.rssiMv - tgt) > 60) {
            return {QStringLiteral("WARN"), where + QStringLiteral(" — проверьте уровень сигнала")};
        }
        return {QStringLiteral("PASS"), where};
    });
    step(QStringLiteral("Видео на выходе (AT7456E STAT)"), [&]() -> StepResult {
        const Status s = readStatus();
        const bool loss = s.flags & video::FlagVideoLoss;
        const QString stat = QStringLiteral("STAT 0x%1").arg(s.osdStat, 0, 16);
        if (!opt.haveVideo) {
            return loss ? StepResult{QStringLiteral("SKIP"), QStringLiteral("видео не ожидалось (LOS)")}
                        : StepResult{QStringLiteral("PASS"), QStringLiteral("синхронизация есть, ") + stat};
        }
        if (loss) {
            fail(QStringLiteral("нет синхронизации (LOS)"));
        }
        return {QStringLiteral("PASS"),
                (s.osdStat & 1 ? QStringLiteral("PAL, ") : s.osdStat & 2 ? QStringLiteral("NTSC, ") : QStringLiteral("?, "))
                    + stat};
    });
    step(QStringLiteral("Запись в OSD"), [&]() -> StepResult {
        const QString txt = (QStringLiteral("TEST ") + opt.serial).left(28);
        const QList<quint16> glyphs = video::glyphsFor(txt, opt.asciiFont);
        QList<quint16> v{1, 1, 1, 1, static_cast<quint16>(glyphs.size()), 0, 1, 0};
        v += glyphs;
        transaction(video::CmdOsdWrite, {{0x0300, v}}, 200);
        const QList<quint16> g = input(0x0304, 1);
        return {QStringLiteral("PASS"), QStringLiteral("APPLIED, %1 симв. (видно на мониторе: «%2»)").arg(g[0]).arg(txt)};
    });
    step(QStringLiteral("Сторожевой таймер связи (1 с → SAFE)"), [&]() -> StepResult {
        applySynth(ext(0, 0, 850));
        pause(1400);  // намеренное молчание: узел обязан уйти в SAFE
        const Status s = readStatus();
        if (s.state != 6 || s.lastError != 9 || !(s.flags & video::FlagWatchdogSafe)) {
            fail(QStringLiteral("STATE=%1, LAST_ERROR=%2").arg(video::stateName(s.state), video::errorName(s.lastError)));
        }
        if (s.attenCode != 63) {
            fail(QStringLiteral("ослабление не максимальное: %1").arg(s.attenCode));
        }
        return {QStringLiteral("PASS"), QStringLiteral("SAFE, WATCHDOG, 31,5 дБ")};
    });
    step(QStringLiteral("Команда SAFE"), [&]() -> StepResult {
        applySynth(ext(0, 0, 850));
        const Status s = transaction(video::CmdSafe, {});
        if (s.state != 6) {
            fail(QStringLiteral("STATE=") + video::stateName(s.state));
        }
        const Status s2 = readStatus();
        if (s2.attenCode != 63 || (s2.flags & video::FlagPllLock)) {
            fail(QStringLiteral("LO/ослабление не в безопасном состоянии"));
        }
        return {QStringLiteral("PASS"), QStringLiteral("LO выкл., 31,5 дБ")};
    });
    step(QStringLiteral("Отказ на недопустимую частоту"), [&]() -> StepResult {
        video::ApplyRequest a;
        a.freqHz = 6500e6;
        a.sendExt = false;
        try {
            applySynth(a);
        } catch (const VideoError& e) {
            if (e.message.contains(QStringLiteral("BAD_VALUE"))) {
                return {QStringLiteral("PASS"), QStringLiteral("COMMIT отклонён, BAD_VALUE")};
            }
            throw;
        }
        fail(QStringLiteral("6500 МГц принята"));
    });

    const QString verdict = stopped ? QStringLiteral("ОСТАНОВЛЕНО") : fails ? QStringLiteral("НЕ ГОДНА") : QStringLiteral("ГОДНА");
    emit testFinished(verdict, fails, warns, st);
}

void VideoSession::scanChannels(int agcMode, int attenCode, int targetMv, int power)
{
    m_abort.store(false);
    int bestMhz = 0;
    int bestRssi = -1;
    QString bestName;
    try {
        for (const auto& band : channelPlan()) {
            for (int i = 0; i < band.second.size(); ++i) {
                if (m_abort.load()) {
                    fail(QStringLiteral("остановлено"));
                }
                // Для сравнения каналов — фиксированное ослабление 0 дБ, без АРУ.
                video::ApplyRequest a;
                a.freqHz = band.second.at(i) * 1e6;
                a.power = power;
                a.agcMode = 1;
                a.attenCode = 0;
                a.targetMv = 850;
                applySynth(a);
                pause(350);
                const Status s = readStatus();
                const QString name = band.first + QString::number(i + 1);
                emit scanRow(name, band.second.at(i), s.rssiMv);
                if (s.rssiMv > bestRssi) {
                    bestRssi = s.rssiMv;
                    bestMhz = band.second.at(i);
                    bestName = name;
                }
            }
        }
        // Вернуть выбранный режим АРУ на сильнейшем канале.
        video::ApplyRequest a;
        a.freqHz = bestMhz * 1e6;
        a.power = power;
        a.agcMode = agcMode;
        a.attenCode = attenCode;
        a.targetMv = targetMv;
        applySynth(a);
        emit scanFinished(bestMhz, QStringLiteral("сильнейший: %1 %2 МГц").arg(bestName).arg(bestMhz));
    } catch (const VideoError& e) {
        emit logLine(QStringLiteral("er"), QStringLiteral("Обзор: ") + e.message);
        emit scanFinished(0, QStringLiteral("обзор прерван: ") + e.message);
    }
}

void VideoSession::waitBoot(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (m_abort.load()) {
            fail(QStringLiteral("остановлено"));
        }
        try {
            if (input(0x0F00, 3, Options{80, 0, false})[0] == 2) {
                return;
            }
        } catch (const VideoError&) {  // узел перезапускается
        }
        pause(60);
    }
    fail(QStringLiteral("загрузчик не ответил"));
}

void VideoSession::waitFwState(int want, int ms)
{
    QElapsedTimer t;
    t.start();
    for (;;) {
        if (m_abort.load()) {
            fail(QStringLiteral("остановлено"));
        }
        try {
            const QList<quint16> r = input(0x0F02, 2, Options{120, 0, false});
            if (r[0] == want) {
                return;
            }
            if (r[0] == 5) {
                throw VideoError{QStringLiteral("загрузчик: ") + video::fwErrorName(r[1]), -2};
            }
        } catch (const VideoError& e) {
            if (e.code == -2) {
                throw;
            }
        }
        if (t.elapsed() > ms) {
            fail(QStringLiteral("тайм-аут ожидания ") + video::fwStateName(want));
        }
        pause(100);
    }
}

Status VideoSession::flashImage(const QByteArray& img, quint32 version)
{
    const auto note = [this](double fraction, const QString& text) {
        emit flashProgress(fraction, text);
        emit logLine(QStringLiteral("in"), text);
    };
    const QList<quint16> enter{1, video::kFwKey};
    int mode = 0;
    try {
        mode = input(0x0F00, 1)[0];
    } catch (const VideoError&) {
    }
    if (mode != 2) {
        note(0, QStringLiteral("Перезапуск в загрузчик…"));
        writeMulti(0x0F00, enter);
        pause(300);
    }
    waitBoot(3000);
    writeMulti(0x0F00, enter);  // остаться в загрузчике
    const int pages = static_cast<int>((img.size() + 1023) / 1024);
    note(0, QStringLiteral("Стирание (%1 страниц)…").arg(pages));
    QList<quint16> begin{2, video::kFwKey};
    begin += video::w32(static_cast<quint32>(img.size()));
    begin += video::w32(video::crc32(img));
    begin += video::w32(version);
    writeMulti(0x0F00, begin);
    pause(50 * pages);
    waitFwState(2, 8000);
    QElapsedTimer t;
    t.start();
    const int size = static_cast<int>(img.size());
    for (int off = 0; off < size; off += 240) {
        if (m_abort.load()) {
            fail(QStringLiteral("остановлено"));
        }
        const int n = std::min(240, size - off);
        QList<quint16> vals = video::w32(static_cast<quint32>(off));
        vals.append(static_cast<quint16>(n));
        for (int i = 0; i < n; i += 2) {
            vals.append(static_cast<quint16>((static_cast<quint8>(img.at(off + i)) << 8)
                                             | static_cast<quint8>(img.at(off + i + 1))));
        }
        writeMulti(0x1000, vals, Options{120, 4, true});
        const double done = static_cast<double>(off + n) / size;
        if ((off / 240) % 8 == 0) {
            note(done, QStringLiteral("Запись: %1 / %2 байт, %3 КБ/с")
                           .arg(off + n).arg(size)
                           .arg((off + n) / std::max(0.001, t.elapsed() / 1000.0) / 1024.0, 0, 'f', 1));
        } else {
            emit flashProgress(done, {});
        }
    }
    note(1, QStringLiteral("Проверка CRC32…"));
    writeMulti(0x0F00, {3, video::kFwKey});
    pause(150);
    waitFwState(4, 5000);
    note(1, QStringLiteral("Запуск приложения…"));
    writeMulti(0x0F00, {4, video::kFwKey});
    pause(900);
    QElapsedTimer run;
    run.start();
    for (;;) {
        try {
            const Status s = readStatus();
            if (s.fwMode == 1) {
                return s;
            }
        } catch (const VideoError&) {
        }
        if (run.elapsed() > 5000) {
            fail(QStringLiteral("приложение не запустилось"));
        }
        pause(200);
    }
}

void VideoSession::flash(const QByteArray& image, quint32 version)
{
    m_abort.store(false);
    // Образ проверяется и здесь: слот могут вызвать мимо проверки вкладки.
    if (image.isEmpty() || image.size() > video::kMaxImageBytes || image.size() % 4 != 0) {
        emit flashFinished(false, QStringLiteral("Ошибка: образ не подходит."));
        return;
    }
    try {
        const Status s = flashImage(image, version);
        emit flashFinished(true, QStringLiteral("Готово: приложение запущено, ПО %1, BOOT_ID %2").arg(s.fwText(), s.bootText()));
    } catch (const VideoError& e) {
        emit flashFinished(false, QStringLiteral("Ошибка: %1. Плата осталась в загрузчике — повторите прошивку.").arg(e.message));
    }
}

void VideoSession::recover()
{
    m_abort.store(false);
    emit flashProgress(0, QStringLiteral("Выключите и включите питание платы… (10 с)"));
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < 10000 && !m_abort.load()) {
        try {
            writeMulti(0x0F00, {1, video::kFwKey}, Options{40, 0, true});
            if (input(0x0F00, 1, Options{60, 0, true})[0] == 2) {
                emit flashFinished(true, QStringLiteral("Плата в загрузчике — можно прошивать."));
                return;
            }
        } catch (const VideoError&) {  // платы ещё нет на шине
        }
        pause(20);
    }
    emit flashFinished(false, m_abort.load() ? QStringLiteral("Восстановление: остановлено")
                                              : QStringLiteral("Восстановление: загрузчик не ответил"));
}
