#pragma once

#include <QHash>
#include <QMap>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

class PsSession;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;

/// «Параметры платы» в окошке «Тех. состояние»: всё, что PS отдаёт о плате, — режим и
/// состояние, версия протокола, температура, напряжения и загрузка процессора из
/// телеметрии, а в сервисном режиме ещё АЦП, луч, гетеродины, смеситель и видеоприёмник.
/// Настройки платы не меняются: уходят только запросы get_*.
///
/// Вкладки: «PS часть», «Ошибки», «Состояние» (трансивер AD9361, ПЛИС, РЧ-модули —
/// кладут «Настройки»), «Подробно». Вкладка «Ошибки» собирает красным отказы PS
/// (статус ответа не OK, например HW_NOT_READY) и неисправности из UDP-пакета
/// технического состояния. Отказ уходит из списка, когда тот же запрос
/// выполнен успешно.
class BoardParametersPage final : public QWidget {
    Q_OBJECT

public:
    explicit BoardParametersPage(PsSession* session, QWidget* parent = nullptr);

    /// Запросить параметры, как по кнопке «Получить параметры».
    void requestRead();
    [[nodiscard]] bool busy() const { return m_busy; }
    /// Добавить вкладку среди результатов (index — позиция, как у QTabWidget::insertTab).
    void insertResultTab(int index, QWidget* page, const QString& title);
    /// Строки вкладки «Ошибки» — отказы PS и неисправности по UDP.
    [[nodiscard]] QStringList errors() const;

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
    void noteRefusal(const QString& title, int status);
    void renderErrors();

    PsSession* m_session;
    QLabel* m_status = nullptr;
    QPushButton* m_read = nullptr;
    QTabWidget* m_results = nullptr;
    QPlainTextEdit* m_main = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QListWidget* m_errors = nullptr;
    /// Отказы PS по названию запроса: время и статус последнего отказа.
    struct Refusal {
        QString time;
        int status = 0;
    };
    QMap<QString, Refusal> m_refusals;
    QTimer* m_timeout = nullptr;
    QHash<QString, Reply> m_pending;   // ключ «тип#номер» запросов, ждущих ответа
    QVector<Reply> m_replies;          // полученные ответы в порядке запроса
    bool m_busy = false;
    bool m_serviceNote = false;
};
