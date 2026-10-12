#pragma once

#include <QWidget>

class PsLink;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

/// Страница «Пакет»: любая команда PS с payload, набранным вручную.
/// Payload не проверяется на клиенте — так проверяют отказы самого PS.
class PsPacketPage final : public QWidget {
    Q_OBJECT

public:
    explicit PsPacketPage(PsLink* link, QWidget* parent = nullptr);

    void setAllowed(bool allowed);
    /// Отправить выбранную команду; false — JSON не объект или нет связи.
    bool sendPacket();

private:
    void loadTemplate();

    PsLink* m_link;
    QComboBox* m_type = nullptr;
    QPlainTextEdit* m_payload = nullptr;
    QPushButton* m_send = nullptr;
    QLabel* m_result = nullptr;
};
