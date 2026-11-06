#pragma once

#include "ChannelRun.h"
#include "Ppkh4Frame.h"

#include <QWidget>

#include <array>
#include <functional>

class Ppkh4Link;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QSlider;
class QDoubleSpinBox;
class QSpinBox;
class QTimer;
class QToolButton;

/// «Настройка 4 каналов»: четыре канала одной SPI-цепочки.
/// Любое изменение сдвигает все четыре слова подряд. Коды помнит программа.
class FourChannelPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FourChannelPanel(QWidget* parent = nullptr);

    [[nodiscard]] int slotIndex() const;
    /// Номер канала цепочки, 1…4.
    [[nodiscard]] int globalChannel() const;
    [[nodiscard]] int phaseCode() const { return m_phCode; }
    [[nodiscard]] int attenCode() const { return m_atCode; }
    [[nodiscard]] int phaseCodeAt(int index) const;
    [[nodiscard]] int attenCodeAt(int index) const;
    /// Компактные регуляторы тех же четырёх каналов. Состояния своего не хранит.
    [[nodiscard]] QWidget* sideControl(QWidget* parent);
    [[nodiscard]] bool measuring() const { return m_running; }
    /// Ответ анализатора на один шаг. Векторы — |S| в дБ по частоте.
    void onSweepFinished(bool ok, const QString& message, const QVector<double>& freqGhz,
                         const QVector<double>& s11mag, const QVector<double>& s21mag,
                         const QVector<double>& s12mag, const QVector<double>& s22mag);

signals:
    /// Нужен один полный снимок S11/S21/S12/S22 текущими настройками свипа.
    void channelSweepRequested();

private:
    QWidget* makeConnectionBox();
    QWidget* makeSelectBox();
    QWidget* makePhaseBox();
    QWidget* makeAttenBox();
    QWidget* makeLogBox();
    QWidget* makeMeasureBox();
    void buildSideControl(QWidget* parent);

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
    void setChannelPhase(int index, int code);
    void setChannelAtten(int index, int code);
    void setChannelOrder(int index, int lsbFirst);
    void render();
    void refreshPacket();
    void applyCfg();
    void sendFrame();
    void sendFrame(const std::function<void(bool)>& after);
    void appendLog(const QString& line);

    void startMeasure(const QVector<int>& channels);
    void stopMeasure(const QString& outcome);
    void continueMeasure();
    void applyMeasureStep();
    void finishMeasurePdf();
    void restoreSavedCodes();
    void setMeasureUi();

    Ppkh4Link* m_link = nullptr;
    afar::ppkh4::FrameFormat m_format;
    afar::ppkh4::LineConfig m_lines;
    std::array<int, 4> m_phase{};
    std::array<int, 4> m_atten{};
    std::array<int, 4> m_lsb{};
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

    QComboBox* m_slot = nullptr;
    QLabel* m_global = nullptr;
    QPushButton* m_apply = nullptr;
    std::array<QPushButton*, 4> m_rows{};

    QLabel* m_packet = nullptr;
    QLabel* m_packetBits = nullptr;
    std::array<QToolButton*, 4> m_bitToggles{};
    std::array<QWidget*, 4> m_bitPanels{};
    std::array<QComboBox*, 4> m_order{};
    std::array<QLabel*, 4> m_bitViews{};

    QWidget* m_side = nullptr;
    std::array<QLabel*, 4> m_sideValue{};
    std::array<QSlider*, 4> m_sidePhaseSlider{};
    std::array<QSpinBox*, 4> m_sidePhase{};
    std::array<QDoubleSpinBox*, 4> m_sidePhaseDeg{};
    std::array<QSlider*, 4> m_sideAttenSlider{};
    std::array<QSpinBox*, 4> m_sideAtten{};
    std::array<QDoubleSpinBox*, 4> m_sideAttenDb{};

    QLabel* m_phVal = nullptr;
    QSlider* m_phRange = nullptr;
    QDoubleSpinBox* m_phDeg = nullptr;
    QSpinBox* m_phNum = nullptr;
    QLabel* m_atVal = nullptr;
    QSlider* m_atRange = nullptr;
    QDoubleSpinBox* m_atDb = nullptr;
    QSpinBox* m_atNum = nullptr;

    QPlainTextEdit* m_log = nullptr;

    bool m_running = false;
    bool m_waitCable = false;
    bool m_stopAsked = false;
    int m_stepIndex = 0;
    int m_cableChannel = -1;
    QString m_runDir;
    QString m_runTitle;
    std::array<int, 4> m_savedPhase{};
    std::array<int, 4> m_savedAtten{};
    QVector<ChannelStep> m_steps;
    QVector<ChannelPoint> m_points;
    std::array<QPushButton*, 4> m_measureOne{};
    QPushButton* m_measureAll = nullptr;
    QPushButton* m_measureStop = nullptr;
    QPushButton* m_measureCable = nullptr;
    QLabel* m_measureStatus = nullptr;
    QProgressBar* m_measureBar = nullptr;
    QTableWidget* m_measureTable = nullptr;
    QLabel* m_measureGraph = nullptr;
};
