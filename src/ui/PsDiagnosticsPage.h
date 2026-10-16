#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <map>
#include <string>
#include <string_view>
#include <vector>

class PsSession;
class QLabel;
class QPushButton;
class QTableWidget;

/// Вкладка «Состояние» в «Модулях»: по кнопке на каждую команду диагностики v8 —
/// get_trx_status (трансивер, таблица 36), get_pl_status (ПЛИС, таблица 37) и
/// get_rfm_telemetry (температуры РЧ-модулей, таблица 27). Только чтение,
/// только в режиме SERVICE.
class PsDiagnosticsPage final : public QWidget {
    Q_OBJECT

public:
    explicit PsDiagnosticsPage(PsSession* session, QWidget* parent = nullptr);

    void setAllowed(bool allowed);
    /// Отправить одну из трёх команд; false — не ушла.
    bool request(std::string_view msgType);
    [[nodiscard]] QTableWidget* table() const { return m_table; }

private:
    struct Row {
        QString name;
        QString value;
        bool alarm = false;
    };

    void onResponse(const afar::ps::Envelope& r, afar::ps::Status status);
    void render();

    PsSession* m_session;
    std::vector<QPushButton*> m_buttons;
    QTableWidget* m_table = nullptr;
    QLabel* m_result = nullptr;
    std::map<int, std::string> m_pending;  ///< msg_count → msg_type
    std::map<std::string, std::vector<Row>> m_rows;  ///< последний ответ по типу
};
