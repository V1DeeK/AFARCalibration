#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QWidget>

#include <vector>

class PsLink;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QTimer;

/// Обмен с PS, как в окне «Тест обмена» РМО: каждая строка — пакет TCP или
/// датаграмма UDP, что ушло и что пришло. По выбранной строке видны
/// отправленная команда и ответ на неё целиком. Только показывает: команды
/// шлют страницы вкладки «Параметры PS».
class PsTrafficView final : public QWidget {
    Q_OBJECT

public:
    explicit PsTrafficView(PsLink* link, QWidget* parent = nullptr);

    struct Frame {
        QDateTime at;
        bool outgoing = false;
        bool udp = false;
        QString line;
        QString msgType;
        QString msgClass;
        int msgCount = -1;
        QString status;   ///< status ответа; «нет ответа» у команды по тайм-ауту
        qint64 ms = -1;   ///< у ответа — время от команды
        int pair = -1;    ///< индекс парного пакета (команда ↔ ответ)
        bool broken = false;
        qint64 clockMs = 0;
    };

    [[nodiscard]] const std::vector<Frame>& frames() const { return m_frames; }
    [[nodiscard]] int visibleRows() const;
    [[nodiscard]] int tcpSent() const { return m_tcpSent; }
    [[nodiscard]] int tcpResponses() const { return m_tcpResponses; }
    [[nodiscard]] int tcpTimeouts() const { return m_tcpTimeouts; }
    [[nodiscard]] int udpReceived() const { return m_udpReceived; }

    /// Обмен в текстовый файл: по строке на пакет, как он шёл по сети.
    bool saveLog(const QString& path, QString* error = nullptr) const;
    void clear();

private:
    void onFrame(bool outgoing, const QString& channel, const QString& line);
    void onTimedOut(const QString& msgType, int msgCount);
    void appendRow(int index);
    void refreshRow(int index);
    void rebuildTable();
    void showFrame(int index);
    void refreshCounters();
    [[nodiscard]] bool passes(const Frame& f) const;
    [[nodiscard]] static QString key(const QString& type, int count);

    PsLink* m_link;
    std::vector<Frame> m_frames;
    QHash<QString, int> m_openCommands;  ///< команда без ответа: ключ → индекс
    QElapsedTimer m_clock;
    int m_tcpSent = 0;
    int m_tcpReceived = 0;
    int m_tcpResponses = 0;
    int m_tcpTimeouts = 0;
    int m_udpReceived = 0;

    QCheckBox* m_showTcp = nullptr;
    QCheckBox* m_showUdp = nullptr;
    QCheckBox* m_showKeepAlive = nullptr;
    QCheckBox* m_follow = nullptr;
    QLabel* m_counters = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_sentTitle = nullptr;
    QPlainTextEdit* m_sent = nullptr;
    QLabel* m_receivedTitle = nullptr;
    QPlainTextEdit* m_received = nullptr;
    QTimer* m_countersTimer = nullptr;
};
