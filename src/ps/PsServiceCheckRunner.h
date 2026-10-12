#pragma once

#include "PsLink.h"
#include "PsServiceCheck.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <vector>

/// Прогон приёмочной проверки сервисного режима по шагам через PsLink.
/// Шаги идут строго по одному: следующий — после ответа или тайм-аута.
/// Шаги UDP ждут датаграмму телеметрии, отправленную PS после начала шага.
class PsServiceCheckRunner final : public QObject {
    Q_OBJECT

public:
    struct Outcome {
        afar::ps::CheckResult result;
        QString request;   ///< отправленная строка JSON
        QString response;  ///< полученный payload JSON
        QString got;       ///< ответ одной строкой
        qint64 ms = 0;
    };

    explicit PsServiceCheckRunner(PsLink* link, QObject* parent = nullptr);

    [[nodiscard]] const std::vector<afar::ps::CheckStep>& steps() const { return m_steps; }
    [[nodiscard]] const std::vector<Outcome>& outcomes() const { return m_outcomes; }
    [[nodiscard]] bool isRunning() const { return m_index >= 0; }
    [[nodiscard]] int currentStep() const { return m_index; }

    /// false — нет связи с PS или проверка уже идёт.
    bool start();
    void stop(const QString& reason);

    /// Отчёт в Markdown для сохранения в файл.
    [[nodiscard]] QString reportMarkdown() const;

signals:
    void stepStarted(int index);
    void stepFinished(int index);
    void finished(const QString& reason);

private:
    void sendCurrent();
    void finishStep(afar::ps::CheckResult result, const QString& got, const QString& response);
    void onResponse(const afar::ps::Envelope& response, afar::ps::Status status);
    void onTimedOut(const QString& msgType, int msgCount);
    void onLinkState(PsLink::State state);
    void onFrame(bool outgoing, const QString& channel, const QString& line);
    void checkCounters();

    PsLink* m_link;
    std::vector<afar::ps::CheckStep> m_steps;
    std::vector<Outcome> m_outcomes;
    int m_index = -1;
    int m_msgCount = -1;
    bool m_waitUdp = false;
    afar::ps::CheckResult m_lastUdp;  ///< последняя датаграмма шага с не тем mode
    QString m_lastUdpText;
    quint64 m_tlmReceived0 = 0;
    quint64 m_tlmLost0 = 0;
    quint64 m_tlmRejected0 = 0;
    QTimer m_guard;
    QElapsedTimer m_stepClock;
    QDateTime m_startedAt;
    QDateTime m_finishedAt;
    QString m_finishReason;
};
