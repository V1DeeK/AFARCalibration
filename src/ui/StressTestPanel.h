#pragma once

#include "PsProtocol.h"

#include <QElapsedTimer>
#include <QHash>
#include <QVector>
#include <QWidget>

class PsSession;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

/// Стресс-тест обмена с PS (перенесён из «Теста обмена» РМО): за заданное время
/// («N команд за T мс/с/мин») по кругу шлёт отмеченные команды и ведёт по каждой счётчики —
/// отправлено, OK, отказ PS, нет ответа, не ушло, время ответа. Каждый пакет
/// на экран не выводится: сводка обновляется 4 раза в секунду, по клику на
/// строку — последний проблемный обмен этой команды.
class StressTestPanel final : public QWidget {
    Q_OBJECT

public:
    struct Command {
        std::string msgType;
        nlohmann::json payload = nlohmann::json::object();
        /// Меняет настройки платы: по умолчанию не отмечена, при отметке — предупреждение.
        bool setter = false;
    };

    StressTestPanel(PsSession* session, const QVector<Command>& commands, QWidget* parent = nullptr);

    [[nodiscard]] bool running() const { return m_running; }
    void start();
    void stop(const QString& why);
    /// Итог одной строкой (для тестов и отчёта).
    [[nodiscard]] QString summary() const;

signals:
    /// Пакет и ответ для панелей «Отправлено» / «Получено».
    void exchangeChosen(const QString& title, const QString& request, const QString& response);
    void runningChanged(bool running);

private:
    struct Stats {
        Command cmd;
        bool enabled = true;
        int sent = 0;
        int ok = 0;
        int refused = 0;
        int timeout = 0;
        int notSent = 0;
        qint64 totalMs = 0;
        qint64 maxMs = 0;
        QString lastProblem;
        QString lastRequest;
        QString lastResponse;
        QString problemRequest;
        QString problemResponse;
    };
    struct InFlight {
        int row = -1;
        QElapsedTimer clock;
        QString request;
    };

    void resetCounters();
    void sendNext();
    void updateWarning();
    [[nodiscard]] qint64 durationMs() const;
    bool sendOne();
    void onResponse(const afar::ps::Envelope& response, afar::ps::Status status);
    void onTimeout(const QString& msgType, int msgCount);
    void onLinkState();
    void refresh();
    void finishIfDone();
    void markProblem(Stats& s, const QString& text, const QString& request, const QString& response);
    [[nodiscard]] int totalSent() const;
    [[nodiscard]] bool rowInFlight(int row) const;
    static QString key(const QString& msgType, int msgCount);

    PsSession* m_session;
    QSpinBox* m_count = nullptr;
    QSpinBox* m_duration = nullptr;
    QComboBox* m_unit = nullptr;
    QLabel* m_warning = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_stop = nullptr;
    QLabel* m_summary = nullptr;
    QTableWidget* m_table = nullptr;
    QTimer* m_sendTimer = nullptr;
    QTimer* m_refreshTimer = nullptr;

    QVector<Stats> m_stats;
    QHash<QString, InFlight> m_inFlight;
    int m_cursor = 0;
    bool m_running = false;
    bool m_finishing = false;
    int m_plannedCount = 0;
    qint64 m_plannedMs = 0;
    QString m_stopReason;
    QElapsedTimer m_runClock;
};
