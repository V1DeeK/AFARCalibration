#pragma once

#include "Ppkh4Frame.h"

#include <QWidget>

#include <array>

class Ppkh4Link;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QDoubleSpinBox;
class QSpinBox;
class QTimer;

/// «Настройка 4 каналов»: четыре платы по четыре канала (изделие 1…16).
/// К C2220 подключён один канал. В плату уходит один кадр выбранного канала,
/// те же команды, что у «Настройка 1 канала». Коды помнит программа.
class FourChannelPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FourChannelPanel(QWidget* parent = nullptr);

    [[nodiscard]] int boardIndex() const;
    [[nodiscard]] int slotIndex() const;
    /// Номер канала изделия, 1…16.
    [[nodiscard]] int globalChannel() const;
    [[nodiscard]] int phaseCode() const { return m_phCode; }
    [[nodiscard]] int attenCode() const { return m_atCode; }

private:
    QWidget* makeConnectionBox();
    QWidget* makeSelectBox();
    QWidget* makePhaseBox();
    QWidget* makeAttenBox();
    QWidget* makeLogBox();

    void refreshPorts();
    void toggleConnection();
    void onOpened();
    void onClosed(const QString& reason);
    void setConnState(const QString& text, bool ok, bool bad);
    void updateButtons();

    void loadFormat();
    void loadSettings();
    void saveSelection() const;
    void saveChannel(int index) const;

    [[nodiscard]] int selectedIndex() const;
    void onSelectionChanged();
    void showSelected(bool sendNow);
    void refreshRows();

    void setPhaseCode(int code);
    void setAttenCode(int code);
    void render();
    void applyCfg();
    void sendFrame();
    void appendLog(const QString& line);

    Ppkh4Link* m_link = nullptr;
    afar::ppkh4::FrameFormat m_format;
    afar::ppkh4::LineConfig m_lines;
    std::array<int, 16> m_phase{};
    std::array<int, 16> m_atten{};
    int m_phCode = 0;
    int m_atCode = 0;
    bool m_connected = false;
    bool m_loading = false;

    QTimer* m_sendDebounce = nullptr;

    QComboBox* m_port = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_connect = nullptr;
    QLabel* m_conn = nullptr;
    QLabel* m_lastSent = nullptr;

    QComboBox* m_board = nullptr;
    QComboBox* m_slot = nullptr;
    QLabel* m_global = nullptr;
    QPushButton* m_apply = nullptr;
    std::array<QPushButton*, 4> m_rows{};

    QLabel* m_phVal = nullptr;
    QSlider* m_phRange = nullptr;
    QDoubleSpinBox* m_phDeg = nullptr;
    QSpinBox* m_phNum = nullptr;
    QLabel* m_atVal = nullptr;
    QSlider* m_atRange = nullptr;
    QDoubleSpinBox* m_atDb = nullptr;
    QSpinBox* m_atNum = nullptr;

    QPlainTextEdit* m_log = nullptr;
};
