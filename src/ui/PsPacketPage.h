#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QVector>
#include <QWidget>

class PsSession;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

/// «Одна команда» — как в «Тесте обмена» РМО: любая команда PS с payload,
/// набранным вручную, история отправленного и пакеты «Отправлено» /
/// «Получено» по выбранной строке. Payload на клиенте не проверяется — так
/// проверяют отказы самого PS.
class PsPacketPage final : public QWidget {
    Q_OBJECT

public:
    explicit PsPacketPage(PsSession* session, QWidget* parent = nullptr);

    void setAllowed(bool allowed);
    /// Отправить выбранную команду; false — JSON не объект или нет связи.
    bool sendPacket();

private:
    enum class Result { Waiting, Ok, Refused, Timeout, LinkLost, NotSent };
    struct Exchange {
        QString msgType;
        int msgCount = -1;
        QDateTime sentAt;
        QElapsedTimer clock;
        QString request;
        QString response;
        Result result = Result::Waiting;
        QString note;
        qint64 answerMs = 0;
    };

    void loadTemplate();
    void onFrame(bool outgoing, const QString& channel, const QString& line);
    void onTimeout(const QString& msgType, int msgCount);
    void onLinkState();
    int findExchange(const QString& msgType, int msgCount) const;
    void addExchange(const Exchange& e);
    void refreshRow(int index);
    void showExchange(int index);
    [[nodiscard]] QString resultText(const Exchange& e) const;

    PsSession* m_session;
    QComboBox* m_type = nullptr;
    QPlainTextEdit* m_payload = nullptr;
    QPushButton* m_send = nullptr;
    QLabel* m_result = nullptr;
    QTableWidget* m_history = nullptr;
    QPlainTextEdit* m_request = nullptr;
    QPlainTextEdit* m_response = nullptr;
    QVector<Exchange> m_exchanges;
};
