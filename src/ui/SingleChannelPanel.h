#pragma once

#include "Ppkh4Frame.h"
#include "Ppkh4State.h"

#include <QWidget>

#include <functional>

class Ppkh4Link;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QTimer;

/// «Настройка 1 канала»: ручная установка фазы и аттенюации одного модуля через
/// плату ППКХ4 (USB COM) и перебор кодов для наблюдения на C2220.
/// Перенесено из репозитория V1DeeK/1Chanel (web/index.html). C2220 не трогает,
/// с серией измерений и оркестратором не связано.
class SingleChannelPanel final : public QWidget {
    Q_OBJECT

public:
    explicit SingleChannelPanel(QWidget* parent = nullptr);
    ~SingleChannelPanel() override;

    [[nodiscard]] int phaseCode() const { return m_phCode; }
    [[nodiscard]] int attenCode() const { return m_atCode; }
    [[nodiscard]] afar::ppkh4::FrameFormat frameFormat() const { return m_format; }
    /// Текущий кадр в виде команды SEND (то, что уйдёт в плату).
    [[nodiscard]] QString sendCommand() const;
    /// Компактное управление тем же каналом для панели графиков.
    QWidget* createQuickControls();

    void setPhaseCode(int code);
    void setAttenCode(int code);

    /// Настройки вкладки для файла состояния стенда.
    [[nodiscard]] afar::ppkh4::ChannelState channelState() const;
    /// Применить сохранённые настройки; при связи с платой сразу шлёт CFG и SEND.
    void applyChannelState(const afar::ppkh4::ChannelState& state);
    [[nodiscard]] bool boardConnected() const;

signals:
    /// Кнопки «Сохранить/Загрузить состояние»: диалоги и S2VNA ведёт MainWindow через MeasureWorker.
    void stateSaveRequested();
    void stateLoadRequested();

private:
    QWidget* makeConnectionBox();
    QWidget* makePhaseBox();
    QWidget* makeAttenBox();
    QWidget* makeSweepBox();
    QWidget* makeFormatBox();
    QWidget* makeLogBox();

    void refreshPorts();
    void toggleConnection();
    void onOpened();
    void onClosed(const QString& reason);
    void setConnState(const QString& text, bool ok, bool bad);
    void updateButtons();

    void loadSettings();
    void saveSettings() const;
    void loadForm();
    void readForm();
    void onFormatEdited();
    void resetFormat();

    void render();
    void changed();
    void applyCfg();
    void sendFrame(const std::function<void()>& after = {});

    void startSweep();
    void stopSweep();
    void sweepTick();
    void updateSweepRows();

    void appendLog(const QString& line);

    Ppkh4Link* m_link = nullptr;
    afar::ppkh4::FrameFormat m_format;
    afar::ppkh4::LineConfig m_lines;
    int m_phCode = 0;
    int m_atCode = 0;
    bool m_connected = false;

    QTimer* m_sendDebounce = nullptr;
    QTimer* m_sweepTimer = nullptr;
    bool m_sweeping = false;
    std::vector<std::pair<int, int>> m_sweepPoints;
    std::size_t m_sweepIndex = 0;

    // Подключение
    QComboBox* m_port = nullptr;
    QPushButton* m_refresh = nullptr;
    QPushButton* m_connect = nullptr;
    QLabel* m_conn = nullptr;
    QLabel* m_lastSent = nullptr;

    // Фаза
    QLabel* m_phVal = nullptr;
    QLabel* m_phCodeLabel = nullptr;
    QSlider* m_phRange = nullptr;
    QDoubleSpinBox* m_phDeg = nullptr;
    QSpinBox* m_phNum = nullptr;

    // Аттенюация
    QLabel* m_atVal = nullptr;
    QLabel* m_atCodeLabel = nullptr;
    QSlider* m_atRange = nullptr;
    QDoubleSpinBox* m_atDb = nullptr;
    QSpinBox* m_atNum = nullptr;
    QSlider* m_quickPhRange = nullptr;
    QDoubleSpinBox* m_quickPhDeg = nullptr;
    QSlider* m_quickAtRange = nullptr;
    QDoubleSpinBox* m_quickAtDb = nullptr;

    // Перебор
    QComboBox* m_swWhat = nullptr;
    QSpinBox* m_swFrom = nullptr;
    QSpinBox* m_swTo = nullptr;
    QSpinBox* m_swStep = nullptr;
    QSpinBox* m_swAtFrom = nullptr;
    QSpinBox* m_swAtTo = nullptr;
    QSpinBox* m_swAtStep = nullptr;
    QWidget* m_swPhaseRow = nullptr;
    QWidget* m_swAttenRow = nullptr;
    QSpinBox* m_swDwell = nullptr;
    QPushButton* m_swStart = nullptr;
    QPushButton* m_swStop = nullptr;
    QLabel* m_swState = nullptr;

    // Формат кадра
    QSpinBox* m_fTotal = nullptr;
    QLineEdit* m_fFixed = nullptr;
    QSpinBox* m_fPhOff = nullptr;
    QSpinBox* m_fPhW = nullptr;
    QDoubleSpinBox* m_fPhLsb = nullptr;
    QCheckBox* m_fPhInv = nullptr;
    QCheckBox* m_fPhRev = nullptr;
    QSpinBox* m_fAtOff = nullptr;
    QSpinBox* m_fAtW = nullptr;
    QDoubleSpinBox* m_fAtLsb = nullptr;
    QCheckBox* m_fAtInv = nullptr;
    QCheckBox* m_fAtRev = nullptr;
    QComboBox* m_cLsb = nullptr;
    QComboBox* m_cClk = nullptr;
    QComboBox* m_cLeAct = nullptr;
    QComboBox* m_cLeMode = nullptr;
    QSpinBox* m_cHalf = nullptr;
    QSpinBox* m_cLeUs = nullptr;
    QLabel* m_overlap = nullptr;
    QLabel* m_bits = nullptr;

    QPlainTextEdit* m_log = nullptr;
    bool m_loading = false;
};
