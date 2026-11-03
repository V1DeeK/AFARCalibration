#include "HeterMeasure.h"

#include "HeterFrame.h"
#include "HeterFsw.h"
#include "HeterReport.h"
#include "HeterSession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QUuid>

#include <cmath>
#include <stdexcept>

namespace {

constexpr int kMaxFreqs = 256;
constexpr int kMaxPoints = 4096;

qint64 hzOfMhz(double mhz)
{
    const qint64 hz = qRound64(mhz * 1e6);
    if (hz < static_cast<qint64>(kHeterMinHz) || hz > static_cast<qint64>(kHeterMaxHz)) {
        throw std::runtime_error("Частота должна быть 51,5625…6600 МГц");
    }
    return hz;
}

int halfDb(double db)
{
    const double steps = db * 2.0;
    const int code = qRound(steps);
    if (code < 0 || code > 63 || std::abs(steps - code) > 1e-4) {
        throw std::runtime_error("Укажите границы ослабления 0…31,5 дБ, кратные 0,5 дБ");
    }
    return code;
}

QStringList routePorts(const QString& route)
{
    if (route == QLatin1String("A-B")) {
        return {QStringLiteral("A"), QStringLiteral("B")};
    }
    if (route == QLatin1String("B-A")) {
        return {QStringLiteral("B"), QStringLiteral("A")};
    }
    if (route == QLatin1String("A") || route == QLatin1String("B")) {
        return {route};
    }
    if (route == QLatin1String("sum")) {
        return {QStringLiteral("Σ")};
    }
    throw std::runtime_error("Выберите, с какого разъёма снимать сигнал");
}

struct Enables {
    bool a = false;
    bool b = false;
};

Enables enablesOf(const QString& name)
{
    if (name == QLatin1String("A")) {
        return {true, false};
    }
    if (name == QLatin1String("B")) {
        return {false, true};
    }
    if (name == QLatin1String("AB")) {
        return {true, true};
    }
    throw std::runtime_error("Выберите, какие каналы включить");
}

QString modeName(const Enables& en)
{
    if (en.a && en.b) {
        return QStringLiteral("A+B");
    }
    return en.a ? QStringLiteral("A") : QStringLiteral("B");
}

QVector<qint64> frequenciesOf(const HeterJob& job)
{
    if (!(job.freqStepMhz > 0) || job.freqMinMhz > job.freqMaxMhz) {
        throw std::runtime_error("Диапазон частоты: «от» не больше «до», шаг больше нуля");
    }
    const qint64 lo = hzOfMhz(job.freqMinMhz);
    const qint64 hi = hzOfMhz(job.freqMaxMhz);
    const qint64 step = qRound64(job.freqStepMhz * 1e6);
    if (step <= 0) {
        throw std::runtime_error("Диапазон частоты: «от» не больше «до», шаг больше нуля");
    }
    QVector<qint64> grid;
    for (qint64 freq = lo; freq < hi;) {
        grid.append(freq);
        if (grid.size() > kMaxFreqs) {
            throw std::runtime_error(QStringLiteral("Слишком много частот, не больше %1").arg(kMaxFreqs).toStdString());
        }
        if (freq > hi - step) {
            break;
        }
        freq += step;
    }
    if (grid.isEmpty() || grid.back() != hi) {
        grid.append(hi);
    }
    QVector<qint64> unique;
    for (qint64 hz : grid) {
        if (!unique.contains(hz)) {
            unique.append(hz);
        }
    }
    return unique;
}

struct Plan {
    QString analyzerIp;
    QString psuIp;
    QString routing;
    QString enables;
    QVector<qint64> frequencies;
    int attMinCode = 0;
    int attMaxCode = 0;
    qint64 spanHz = 0;
    QStringList ports;
    Enables on;
};

Plan parse(const HeterJob& job)
{
    Plan plan;
    plan.ports = routePorts(job.routing);
    plan.routing = job.routing;
    plan.on = enablesOf(job.enables);
    plan.enables = job.enables;
    plan.frequencies = frequenciesOf(job);
    if (job.attMinDb > job.attMaxDb) {
        throw std::runtime_error("Укажите границы ослабления 0…31,5 дБ, кратные 0,5 дБ");
    }
    plan.attMinCode = halfDb(job.attMinDb);
    plan.attMaxCode = halfDb(job.attMaxDb);
    plan.spanHz = qRound64(job.spanMhz * 1e6);
    if (plan.spanHz < 2000000 || plan.spanHz > 200000000) {
        throw std::runtime_error("Обзор должен быть 2…200 МГц, нижняя частота поиска — выше нуля");
    }
    for (qint64 freq : plan.frequencies) {
        if (freq <= plan.spanHz / 2) {
            throw std::runtime_error("Обзор должен быть 2…200 МГц, нижняя частота поиска — выше нуля");
        }
    }
    const int steps = plan.attMaxCode - plan.attMinCode + 1;
    if (steps * plan.frequencies.size() * plan.ports.size() > kMaxPoints) {
        throw std::runtime_error(
            QStringLiteral("Слишком много точек, не больше %1. Увеличьте шаг или сузьте диапазон").arg(kMaxPoints).toStdString());
    }
    QHostAddress analyzer;
    if (!analyzer.setAddress(job.analyzerIp.trimmed()) || analyzer.protocol() != QAbstractSocket::IPv4Protocol) {
        throw std::runtime_error("Укажите IPv4-адрес прибора");
    }
    plan.analyzerIp = analyzer.toString();
    if (!job.psuIp.trimmed().isEmpty()) {
        QHostAddress psu;
        if (!psu.setAddress(job.psuIp.trimmed()) || psu.protocol() != QAbstractSocket::IPv4Protocol) {
            throw std::runtime_error("Укажите IPv4-адрес прибора");
        }
        plan.psuIp = psu.toString();
    }
    return plan;
}

QString nowText()
{
    return QDateTime::currentDateTime().toOffsetFromUtc(QDateTime::currentDateTime().offsetFromUtc()).toString(Qt::ISODate);
}

HeterReply must(HeterSession* session, quint8 func, const QByteArray& data)
{
    const HeterReply reply = session->transact(func, data);
    if (!reply.ok) {
        throw std::runtime_error((reply.text.isEmpty() ? QStringLiteral("плата не подтвердила команду") : reply.text).toStdString());
    }
    return reply;
}

void sendConfig(HeterSession* session, const HeterState& state, bool a, bool b, bool rf, bool ce)
{
    const int codeA = heter::powerCode(state.aDbm);
    const int codeB = heter::powerCode(state.bDbm);
    const quint16 word = static_cast<quint16>((state.sw2 ? 1 : 0) | ((ce ? 1 : 0) << 1) | ((rf ? 1 : 0) << 2)
                                              | ((a ? 1 : 0) << 3) | ((b ? 1 : 0) << 4)
                                              | (static_cast<quint16>(qMax(0, codeA)) << 8)
                                              | (static_cast<quint16>(qMax(0, codeB)) << 10));
    must(session, 0x01, heter::packU16(word));
}

void sendAtt(HeterSession* session, double a, double b)
{
    QByteArray data(2, 0);
    data[0] = static_cast<char>(qRound(a * 2.0));
    data[1] = static_cast<char>(qRound(b * 2.0));
    must(session, 0x04, data);
}

HeterState fresh(HeterSession* session)
{
    const HeterReply reply = must(session, 0x05, {});
    return reply.state.connected ? reply.state : session->state();
}

bool sameSetup(const HeterState& got, qint64 freq, double attA, double attB, const Enables& on, const HeterState& initial)
{
    return got.fOutHz == static_cast<quint64>(freq) && std::abs(got.attDb[0] - attA) < 0.01
           && std::abs(got.attDb[1] - attB) < 0.01 && got.aEn == on.a && got.bEn == on.b && got.ce && got.enRf
           && got.aDbm == initial.aDbm && got.bDbm == initial.bDbm && got.sw2 == initial.sw2;
}

}  // namespace

HeterMeasure::HeterMeasure(HeterSession* session, QObject* parent)
    : QObject(parent)
    , m_session(session)
{
    qRegisterMetaType<HeterRunStatus>();
}

QString HeterMeasure::explain(const HeterJob& job, int* points)
{
    try {
        const Plan plan = parse(job);
        const int count = (plan.attMaxCode - plan.attMinCode + 1) * plan.frequencies.size() * plan.ports.size();
        if (points != nullptr) {
            *points = count;
        }
        return {};
    } catch (const std::exception& error) {
        if (points != nullptr) {
            *points = 0;
        }
        return QString::fromUtf8(error.what());
    }
}

HeterRunStatus HeterMeasure::status() const
{
    QMutexLocker lock(&m_guard);
    return m_status;
}

bool HeterMeasure::active() const
{
    QMutexLocker lock(&m_guard);
    return m_status.status == QLatin1String("running") || m_status.status == QLatin1String("waiting")
           || m_status.status == QLatin1String("finishing");
}

void HeterMeasure::publish(const HeterRunStatus& status)
{
    {
        QMutexLocker lock(&m_guard);
        m_status = status;
    }
    emit statusChanged(status);
}

void HeterMeasure::requestCancel()
{
    HeterRunStatus copy;
    {
        QMutexLocker lock(&m_guard);
        m_stopping = true;
        if (m_status.status == QLatin1String("running") || m_status.status == QLatin1String("waiting")) {
            m_status.detail = QStringLiteral("Остановка серии");
        }
        copy = m_status;
        m_wait.wakeAll();
    }
    emit statusChanged(copy);
}

void HeterMeasure::requestResume(const QString& runId, const QString& port)
{
    QMutexLocker lock(&m_guard);
    if (runId == m_status.id && m_status.status == QLatin1String("waiting") && port == m_status.port) {
        m_status.status = QStringLiteral("running");
        m_wait.wakeAll();
    }
}

void HeterMeasure::checkpoint()
{
    QMutexLocker lock(&m_guard);
    if (m_stopping) {
        throw Cancelled();
    }
}

void HeterMeasure::waitCable(const QString& port)
{
    HeterRunStatus copy;
    {
        QMutexLocker lock(&m_guard);
        m_status.status = QStringLiteral("waiting");
        m_status.port = port;
        m_status.detail = QStringLiteral("Переставьте кабель анализатора на выход %1").arg(port);
        copy = m_status;
    }
    emit statusChanged(copy);
    QMutexLocker lock(&m_guard);
    while (!m_stopping && m_status.status == QLatin1String("waiting")) {
        m_wait.wait(&m_guard);
    }
    if (m_stopping) {
        throw Cancelled();
    }
}

void HeterMeasure::identify(const QString& ip, const QString& device)
{
    if (active()) {
        emit identified(QStringLiteral("Серия уже выполняется"), false);
        return;
    }
    try {
        emit identified(HeterFsw::identify(ip.trimmed(), device), true);
    } catch (const std::exception& error) {
        emit identified(QString::fromUtf8(error.what()), false);
    }
}

void HeterMeasure::tune(const QString& ip, double centerMhz)
{
    if (active()) {
        emit tuned(QStringLiteral("анализатор занят серией измерений"), false);
        return;
    }
    if (ip.trimmed().isEmpty()) {
        emit tuned(QStringLiteral("IP анализатора не задан"), false);
        return;
    }
    try {
        HeterFsw::retune(ip.trimmed(), centerMhz * 1e6);
        emit tuned(QStringLiteral("анализатор перестроен на %1 МГц").arg(centerMhz, 0, 'f', 4), true);
    } catch (const std::exception& error) {
        emit tuned(QString::fromUtf8(error.what()), false);
    }
}

void HeterMeasure::start(const HeterJob& job, const QString& reportDir)
{
    if (active()) {
        HeterRunStatus busy = status();
        busy.detail = QStringLiteral("Серия уже выполняется");
        emit statusChanged(busy);
        return;
    }
    Plan plan;
    try {
        plan = parse(job);
    } catch (const std::exception& error) {
        HeterRunStatus failed;
        failed.status = QStringLiteral("failed");
        failed.detail = QString::fromUtf8(error.what());
        publish(failed);
        return;
    }

    const int steps = plan.attMaxCode - plan.attMinCode + 1;
    const int total = steps * plan.frequencies.size() * plan.ports.size();
    const QString runId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")) + QLatin1Char('-')
                          + QUuid::createUuid().toString(QUuid::Id128).left(8);
    const QString folder = QDir(reportDir).filePath(runId);
    QDir().mkpath(folder);

    {
        QMutexLocker lock(&m_guard);
        m_stopping = false;
        m_status = {};
        m_status.id = runId;
        m_status.status = QStringLiteral("running");
        m_status.total = total;
        m_status.detail = QStringLiteral("Подключение к FSW");
    }
    emit statusChanged(status());

    QVector<HeterReportPoint> rows;
    HeterReportRun report;
    report.id = runId;
    report.analyzerIp = plan.analyzerIp;
    report.psuIp = plan.psuIp;
    report.routing = plan.routing;
    report.attMin = plan.attMinCode / 2.0;
    report.attMax = plan.attMaxCode / 2.0;
    report.spanHz = plan.spanHz;
    report.total = total;
    report.started = nowText();
    QStringList mhz;
    for (qint64 hz : plan.frequencies) {
        mhz << QString::number(hz / 1e6, 'g', 10);
    }
    report.frequencies = mhz.join(QStringLiteral(", "));

    QJsonObject planJson;
    planJson.insert(QStringLiteral("analyzer_ip"), plan.analyzerIp);
    planJson.insert(QStringLiteral("psu_ip"), plan.psuIp);
    planJson.insert(QStringLiteral("routing"), plan.routing);
    planJson.insert(QStringLiteral("enables"), plan.enables);
    QJsonArray freqs;
    for (qint64 hz : plan.frequencies) {
        freqs.append(static_cast<double>(hz));
    }
    planJson.insert(QStringLiteral("frequencies"), freqs);
    planJson.insert(QStringLiteral("att_min"), report.attMin);
    planJson.insert(QStringLiteral("att_max"), report.attMax);
    planJson.insert(QStringLiteral("span_hz"), static_cast<double>(plan.spanHz));
    QFile planFile(folder + QStringLiteral("/plan.json"));
    if (planFile.open(QIODevice::WriteOnly)) {
        planFile.write(QJsonDocument(planJson).toJson(QJsonDocument::Indented));
    }

    QString outcome = QStringLiteral("done");
    QString error;
    bool dirty = false;
    HeterState initial;
    const Enables on = plan.on;
    const QString mode = modeName(on);

    try {
        HeterFsw fsw(plan.analyzerIp);
        report.instrument = fsw.idn();
        checkpoint();
        initial = fresh(m_session);
        report.haveInitial = true;
        report.rs485Port = initial.port;
        report.externalRef = initial.sw2;
        report.aDbm = initial.aDbm;
        report.bDbm = initial.bDbm;
        dirty = true;
        QFile log(folder + QStringLiteral("/points.jsonl"));
        if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) {
            // Точки остаются в PDF: журнал points.jsonl в этом прогоне недоступен.
        }
        try {
            for (int pass = 0; pass < plan.ports.size(); ++pass) {
                checkpoint();
                const QString port = plan.ports.at(pass);
                sendConfig(m_session, initial, false, false, false, true);
                sendAtt(m_session, 31.5, 31.5);
                if (pass > 0) {
                    waitCable(port);
                }
                for (qint64 freq : plan.frequencies) {
                    checkpoint();
                    sendAtt(m_session, 31.5, 31.5);
                    must(m_session, 0x02, heter::packU64(static_cast<quint64>(freq)));
                    sendConfig(m_session, initial, on.a, on.b, true, true);
                    for (int code = plan.attMaxCode; code >= plan.attMinCode; --code) {
                        checkpoint();
                        const double step = code / 2.0;
                        const double attA = on.a ? step : 31.5;
                        const double attB = on.b ? step : 31.5;
                        HeterRunStatus progress = status();
                        progress.detail = QStringLiteral("Кабель %1 · включены %2 · %3 МГц · %4 дБ")
                                              .arg(port, mode)
                                              .arg(freq / 1e6, 0, 'g', 8)
                                              .arg(step, 0, 'g', 4);
                        progress.status = QStringLiteral("running");
                        publish(progress);
                        sendAtt(m_session, attA, attB);
                        QThread::msleep(120);
                        const HeterState read = fresh(m_session);
                        if (!sameSetup(read, freq, attA, attB, on, initial)) {
                            throw std::runtime_error("Параметры платы не совпали с заданием");
                        }
                        const HeterPeak peak = fsw.measure(static_cast<double>(freq), static_cast<double>(plan.spanHz));
                        HeterReportPoint row;
                        row.n = rows.size() + 1;
                        row.port = port;
                        row.mode = mode;
                        row.attA = attA;
                        row.attB = attB;
                        row.stepDb = step;
                        row.setHz = static_cast<double>(freq);
                        row.peakHz = peak.peakHz;
                        row.levelDbm = peak.levelDbm;
                        row.signal = peak.signal;
                        row.lock = read.lock;
                        row.spanHz = peak.spanHz;
                        row.rbwHz = peak.rbwHz;
                        row.time = nowText();
                        row.imageFile = QStringLiteral("point-%1.png").arg(row.n, 5, 10, QLatin1Char('0'));
                        const QByteArray png = fsw.screenshot();
                        QFile image(folder + QLatin1Char('/') + row.imageFile);
                        if (!image.open(QIODevice::WriteOnly) || image.write(png) != png.size()) {
                            throw std::runtime_error("не удалось сохранить снимок FSW");
                        }
                        QJsonObject point;
                        point.insert(QStringLiteral("n"), row.n);
                        point.insert(QStringLiteral("port"), row.port);
                        point.insert(QStringLiteral("mode"), row.mode);
                        point.insert(QStringLiteral("set_hz"), row.setHz);
                        point.insert(QStringLiteral("step_db"), row.stepDb);
                        point.insert(QStringLiteral("att_a"), row.attA);
                        point.insert(QStringLiteral("att_b"), row.attB);
                        point.insert(QStringLiteral("lock"), row.lock);
                        point.insert(QStringLiteral("temp_c"), read.tempValid ? QJsonValue(read.tempC) : QJsonValue());
                        point.insert(QStringLiteral("time"), row.time);
                        point.insert(QStringLiteral("image"), row.imageFile);
                        point.insert(QStringLiteral("peak_hz"), row.peakHz);
                        point.insert(QStringLiteral("level_dbm"), row.levelDbm);
                        point.insert(QStringLiteral("contrast_db"), peak.contrastDb);
                        point.insert(QStringLiteral("span_hz"), row.spanHz);
                        point.insert(QStringLiteral("rbw_hz"), row.rbwHz);
                        point.insert(QStringLiteral("signal"), row.signal);
                        if (log.isOpen()) {
                            log.write(QJsonDocument(point).toJson(QJsonDocument::Compact));
                            log.write("\n");
                            log.flush();
                        }
                        rows.append(row);
                        progress = status();
                        progress.done = row.n;
                        progress.lastLine = QStringLiteral("Последняя точка №%1 · выход %2 · включены %3 · пик %4 МГц · %5 дБм · LOCK=%6%7")
                                                .arg(row.n)
                                                .arg(row.port, row.mode)
                                                .arg(row.peakHz / 1e6, 0, 'f', 6)
                                                .arg(row.levelDbm, 0, 'f', 2)
                                                .arg(row.lock ? 1 : 0)
                                                .arg(row.signal ? QString() : QStringLiteral(" · слабый пик"));
                        publish(progress);
                    }
                }
            }
            checkpoint();
        } catch (...) {
            try {
                HeterRunStatus restoring = status();
                restoring.status = QStringLiteral("finishing");
                restoring.detail = QStringLiteral("Восстановление платы");
                publish(restoring);
                sendConfig(m_session, initial, false, false, false, true);
                must(m_session, 0x02, heter::packU64(initial.fOutHz));
                sendAtt(m_session, initial.attDb[0], initial.attDb[1]);
                sendConfig(m_session, initial, initial.aEn, initial.bEn, initial.enRf, initial.ce);
                const HeterState back = fresh(m_session);
                if (back.fOutHz != initial.fOutHz || back.sw2 != initial.sw2 || back.ce != initial.ce
                    || back.enRf != initial.enRf || back.aEn != initial.aEn || back.bEn != initial.bEn
                    || std::abs(back.attDb[0] - initial.attDb[0]) > 0.01
                    || std::abs(back.attDb[1] - initial.attDb[1]) > 0.01 || back.aDbm != initial.aDbm
                    || back.bDbm != initial.bDbm) {
                    throw std::runtime_error("контрольное чтение платы отличается от исходных настроек");
                }
                dirty = false;
            } catch (const std::exception& restoreError) {
                error = QString::fromUtf8(restoreError.what())
                        + QStringLiteral(". Восстановление исходных параметров платы не подтверждено.");
            }
            throw;
        }
        {
            HeterRunStatus restoring = status();
            restoring.status = QStringLiteral("finishing");
            restoring.detail = QStringLiteral("Восстановление платы");
            publish(restoring);
            sendConfig(m_session, initial, false, false, false, true);
            must(m_session, 0x02, heter::packU64(initial.fOutHz));
            sendAtt(m_session, initial.attDb[0], initial.attDb[1]);
            sendConfig(m_session, initial, initial.aEn, initial.bEn, initial.enRf, initial.ce);
            const HeterState back = fresh(m_session);
            if (back.fOutHz != initial.fOutHz || back.sw2 != initial.sw2 || back.ce != initial.ce
                || back.enRf != initial.enRf || back.aEn != initial.aEn || back.bEn != initial.bEn
                || std::abs(back.attDb[0] - initial.attDb[0]) > 0.01 || std::abs(back.attDb[1] - initial.attDb[1]) > 0.01
                || back.aDbm != initial.aDbm || back.bDbm != initial.bDbm) {
                throw std::runtime_error("контрольное чтение платы отличается от исходных настроек");
            }
            dirty = false;
        }
    } catch (const Cancelled&) {
        outcome = QStringLiteral("cancelled");
    } catch (const std::exception& failure) {
        outcome = QStringLiteral("failed");
        if (!error.isEmpty()) {
            error = QString::fromUtf8(failure.what()) + QStringLiteral(". ") + error;
        } else {
            error = QString::fromUtf8(failure.what());
        }
        if (dirty) {
            error += QStringLiteral(". Восстановление исходных параметров платы не подтверждено.");
        }
    }

    HeterRunStatus saving = status();
    saving.status = QStringLiteral("finishing");
    saving.detail = QStringLiteral("Сохранение протокола");
    publish(saving);

    report.ended = nowText();
    report.outcome = outcome == QLatin1String("done") ? QStringLiteral("Серия завершена")
                     : outcome == QLatin1String("cancelled") ? QStringLiteral("Серия остановлена")
                                                             : QStringLiteral("Ошибка измерения");
    report.error = error;
    QJsonObject result;
    result.insert(QStringLiteral("id"), runId);
    result.insert(QStringLiteral("outcome"), report.outcome);
    result.insert(QStringLiteral("error"), error);
    result.insert(QStringLiteral("captured"), rows.size());
    result.insert(QStringLiteral("started"), report.started);
    result.insert(QStringLiteral("ended"), report.ended);
    QFile resultFile(folder + QStringLiteral("/result.json"));
    if (resultFile.open(QIODevice::WriteOnly)) {
        resultFile.write(QJsonDocument(result).toJson(QJsonDocument::Indented));
    }
    QString pdfError;
    if (!rows.isEmpty()) {
        pdfError = writeHeterodyneReport(folder, report, rows);
    }
    HeterRunStatus done = status();
    done.status = pdfError.isEmpty() ? outcome : QStringLiteral("failed");
    if (!pdfError.isEmpty()) {
        error += QStringLiteral(" Не удалось сохранить отчёт: %1. Папка: %2").arg(pdfError, folder);
        done.status = QStringLiteral("failed");
    }
    done.detail = error.isEmpty() ? report.outcome : error;
    done.pdfPath = rows.isEmpty() || !pdfError.isEmpty() ? QString() : folder + QStringLiteral("/report.pdf");
    done.done = rows.size();
    done.total = total;
    publish(done);
}
