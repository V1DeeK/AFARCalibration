#pragma once

#include <QHash>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

class PsSession;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;

/// «Параметры платы» в окошке «Тех. состояние»: всё, что PS отдаёт о плате, — режим и
/// состояние, версия протокола, температура, напряжения и загрузка процессора из
/// телеметрии, а в сервисном режиме ещё АЦП, луч, гетеродины, смеситель и видеоприёмник.
/// Настройки платы не меняются: уходят только запросы get_*.
class BoardParametersPage final : public QWidget {
    Q_OBJECT

public:
    explicit BoardParametersPage(PsSession* session, QWidget* parent = nullptr);

    /// Запросить параметры, как по кнопке «Получить параметры».
    void requestRead();
    [[nodiscard]] bool busy() const { return m_busy; }

signals:
    /// Ход операции для полосы состояния главного окна.
    void operationProgress(const QString& text, int completed, int total);

private:
    struct Reply {
        QString title;
        int status = -1;  // PS-статус ответа, -1 — ответа ещё нет
        nlohmann::json payload = nlohmann::json::object();
    };

    void onResponse(const QString& msgType, int msgCount, int status, const nlohmann::json& payload);
    void finish(const QString& note);
    void render();

    PsSession* m_session;
    QLabel* m_status = nullptr;
    QPushButton* m_read = nullptr;
    QTabWidget* m_results = nullptr;
    QPlainTextEdit* m_main = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QTimer* m_timeout = nullptr;
    QHash<QString, Reply> m_pending;   // ключ «тип#номер» запросов, ждущих ответа
    QVector<Reply> m_replies;          // полученные ответы в порядке запроса
    bool m_busy = false;
    bool m_serviceNote = false;
};
