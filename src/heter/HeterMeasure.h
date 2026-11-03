#pragma once

#include "HeterTypes.h"

#include <QMutex>
#include <QObject>
#include <QWaitCondition>

#include <exception>

class HeterSession;

struct HeterJob {
    QString analyzerIp;
    QString psuIp;
    QString routing = QStringLiteral("B");
    QString enables = QStringLiteral("B");
    double freqMinMhz = 5000;
    double freqMaxMhz = 5000;
    double freqStepMhz = 10;
    double attMinDb = 0;
    double attMaxDb = 31.5;
    double spanMhz = 100;
};

/// Серия измерений гетеродина с R&S FSW. Логика — emu/measurement.py.
/// Объект живёт в отдельном потоке; отмена и подтверждение кабеля вызываются из интерфейса напрямую.
class HeterMeasure final : public QObject {
    Q_OBJECT

public:
    explicit HeterMeasure(HeterSession* session, QObject* parent = nullptr);

    /// Пустая строка — план годится. Иначе текст, который видит оператор.
    [[nodiscard]] static QString explain(const HeterJob& job, int* points = nullptr);
    [[nodiscard]] HeterRunStatus status() const;

    void requestCancel();
    void requestResume(const QString& runId, const QString& port);

public slots:
    void identify(const QString& ip, const QString& device);
    void tune(const QString& ip, double centerMhz);
    void start(const HeterJob& job, const QString& reportDir);

signals:
    void identified(const QString& text, bool ok);
    void tuned(const QString& text, bool ok);
    void statusChanged(const HeterRunStatus& status);

private:
    class Cancelled : public std::exception {};

    void checkpoint();
    void publish(const HeterRunStatus& status);
    void waitCable(const QString& port);
    bool active() const;

    HeterSession* m_session = nullptr;
    mutable QMutex m_guard;
    QWaitCondition m_wait;
    HeterRunStatus m_status;
    bool m_stopping = false;
};
