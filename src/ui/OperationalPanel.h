#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <functional>

class PsSession;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

/// «Оперативный режим» (v8 п. 3.5): кнопка на каждую команду — частота и
/// калибровка, политика захвата и сброса, управление лучом, источник пеленга,
/// поиск, захват, сброс, стоп, удержание азимута — и таблицы событий
/// send_op_survey_result и send_op_track_result. Если изделие не в режиме
/// OPERATIONAL, панель сначала переводит его туда, как «Полное сканирование».
class OperationalPanel final : public QWidget {
    Q_OBJECT

public:
    explicit OperationalPanel(PsSession* session, QWidget* parent = nullptr);

    /// Строк в таблице сопровождения не больше — старые уходят.
    static constexpr int kTrackRows = 200;

private:
    QWidget* buildSettings();
    QWidget* buildActions();
    QWidget* buildResults();

    /// Выполнить действие в режиме OPERATIONAL, при необходимости сменив режим.
    void inOpMode(std::function<void()> action);
    void send(std::string_view msgType, const nlohmann::json& payload = nlohmann::json::object());
    void onResponse(const afar::ps::Envelope& response, afar::ps::Status status);
    void onEvent(const afar::ps::Envelope& event);
    void refreshControls();

    PsSession* m_session;
    QLabel* m_mode = nullptr;
    QPushButton* m_toOp = nullptr;

    QDoubleSpinBox* m_freq = nullptr;  ///< МГц
    QComboBox* m_acq = nullptr;
    QComboBox* m_loss = nullptr;
    QComboBox* m_rayMode = nullptr;
    QDoubleSpinBox* m_azMin = nullptr;
    QDoubleSpinBox* m_azMax = nullptr;
    QDoubleSpinBox* m_azFix = nullptr;
    QComboBox* m_data = nullptr;
    QLabel* m_cal = nullptr;
    QDoubleSpinBox* m_acquireAz = nullptr;
    QDoubleSpinBox* m_holdAz = nullptr;
    QLabel* m_hold = nullptr;
    QLabel* m_state = nullptr;

    QTableWidget* m_survey = nullptr;
    QTableWidget* m_track = nullptr;

    std::vector<QPushButton*> m_buttons;
    std::function<void()> m_afterModeChange;
};
