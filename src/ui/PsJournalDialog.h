#pragma once

#include <QDialog>

class PsExchangeView;
class PsSession;
class QComboBox;
class QLabel;
class QLineEdit;

/// Модальное окно «Журнал» вкладки «Подключение»: весь обмен с PS по TCP и UDP
/// и ручная отправка команды с payload одной строкой, как журнал консоли PS
/// в РМО.
class PsJournalDialog final : public QDialog {
    Q_OBJECT

public:
    explicit PsJournalDialog(PsSession* session, QWidget* parent = nullptr);

    [[nodiscard]] PsExchangeView* exchange() const { return m_exchange; }

private:
    void sendRaw();

    PsSession* m_session;
    PsExchangeView* m_exchange = nullptr;
    QComboBox* m_type = nullptr;
    QLineEdit* m_payload = nullptr;
    QLabel* m_result = nullptr;
};
