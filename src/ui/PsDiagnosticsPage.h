#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <map>
#include <string>
#include <string_view>
#include <vector>

class PsSession;
class QLabel;
class QCheckBox;
class QPushButton;
class QSpinBox;
class QTimer;
class QTableWidget;

/// Вкладка «Состояние» в «Тех. состоянии»: по кнопке на каждую команду диагностики v8 —
/// get_trx_status (трансивер AD9361, таблица 36), get_pl_status (ПЛИС, таблица 37) и
/// get_rfm_telemetry (температуры РЧ-модулей, таблица 27) — и опрос AD9361 по таймеру
/// (get_trx_status каждые N с). Только чтение, только в режиме SERVICE.
class PsDiagnosticsPage final : public QWidget {
    Q_OBJECT

public:
    explicit PsDiagnosticsPage(PsSession* session, QWidget* parent = nullptr);

    void setAllowed(bool allowed);
    /// Отправить одну из трёх команд; false — не ушла.
    bool request(std::string_view msgType);
    [[nodiscard]] QTableWidget* table() const { return m_table; }
    /// Опрос AD9361: get_trx_status каждые seconds с, пока разрешено.
    void setPolling(bool on, int seconds = kPollDefaultS);
    [[nodiscard]] bool polling() const;

    static constexpr int kPollMinS = 1;
    static constexpr int kPollMaxS = 60;
    static constexpr int kPollDefaultS = 2;

private:
    struct Row {
        QString name;
        QString value;
        bool alarm = false;
    };

    void onResponse(const afar::ps::Envelope& r, afar::ps::Status status);
    void render();
    void poll();

    PsSession* m_session;
    std::vector<QPushButton*> m_buttons;
    QTableWidget* m_table = nullptr;
    QLabel* m_result = nullptr;
    QCheckBox* m_poll = nullptr;
    QSpinBox* m_pollSeconds = nullptr;
    QTimer* m_pollTimer = nullptr;
    bool m_allowed = false;
    std::map<int, std::string> m_pending;  ///< msg_count → msg_type
    std::map<std::string, std::vector<Row>> m_rows;  ///< последний ответ по типу
};
