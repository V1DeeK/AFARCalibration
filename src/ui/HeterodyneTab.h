#pragma once

#include "heter/HeterTypes.h"

#include <QWidget>

class HeterMeasure;
class HeterSession;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QThread;
class StatTile;

/// Вкладка «Настройка гетеродина»: управление платой ADF4355-3 и серия измерений с R&S FSW.
/// Поведение совпадает с веб-интерфейсом V1DeeK/Heterodyne (emu/ui).
class HeterodyneTab final : public QWidget {
    Q_OBJECT

public:
    explicit HeterodyneTab(QWidget* parent = nullptr);
    ~HeterodyneTab() override;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

private:
    QWidget* buildConnection();
    QWidget* buildFirmware();
    QWidget* buildFrequency();
    QWidget* buildAttenuators();
    QWidget* buildMode();
    QWidget* buildMeasurePage();
    QWidget* buildParameters();
    QWidget* buildJournal();
    void refreshPorts();
    void connectLink();
    void applyFrequency(double mhz);
    void startFlash();
    void setControlsEnabled(bool enabled);
    void applyState(const HeterState& state);
    void renderSchema(const HeterState& state);
    void updatePlanHint();
    void refreshArchive();
    [[nodiscard]] QString reportDirectory() const;

    QThread* m_thread = nullptr;
    HeterSession* m_session = nullptr;
    HeterMeasure* m_measure = nullptr;
    bool m_filling = false;
    bool m_filled = false;
    bool m_runActive = false;
    bool m_userClosed = false;
    QString m_linkError;

    QComboBox* m_port = nullptr;
    QComboBox* m_addr = nullptr;
    QPushButton* m_connect = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_clock = nullptr;
    QLabel* m_fswStatus = nullptr;
    QLabel* m_psuStatus = nullptr;
    QLineEdit* m_firmwarePath = nullptr;
    QComboBox* m_firmwareMethod = nullptr;
    QPushButton* m_flash = nullptr;
    QProgressBar* m_flashBar = nullptr;
    QLabel* m_flashStatus = nullptr;
    bool m_flashing = false;
    QString m_identifyKind;
    QLabel* m_freqHint = nullptr;

    QDoubleSpinBox* m_freq = nullptr;
    QComboBox* m_ref = nullptr;
    QComboBox* m_pwrA = nullptr;
    QComboBox* m_pwrB = nullptr;
    QCheckBox* m_enA = nullptr;
    QCheckBox* m_enB = nullptr;
    QCheckBox* m_enRf = nullptr;
    QCheckBox* m_enSyn = nullptr;
    QComboBox* m_cp = nullptr;
    QSlider* m_att1 = nullptr;
    QSlider* m_att2 = nullptr;
    QLabel* m_att1Value = nullptr;
    QLabel* m_att2Value = nullptr;
    QWidget* m_controls = nullptr;

    QWidget* m_schema = nullptr;
    QWidget* m_boardLive = nullptr;
    QLabel* m_boardEmpty = nullptr;
    StatTile* m_tileFreq = nullptr;
    StatTile* m_tileLock = nullptr;
    StatTile* m_tileTemp = nullptr;
    StatTile* m_tileRef = nullptr;
    StatTile* m_tileSynth = nullptr;
    QLabel* m_outA = nullptr;
    QLabel* m_outB = nullptr;
    QLabel* m_regIntro = nullptr;
    QFrame* m_regCard[13] = {};
    QLabel* m_regTitle[13] = {};
    QLabel* m_regBody[13] = {};
    QLabel* m_tempValue = nullptr;
    QPlainTextEdit* m_result = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QCheckBox* m_showRs = nullptr;

    QLineEdit* m_fswIp = nullptr;
    QLineEdit* m_psuIp = nullptr;
    QComboBox* m_enables = nullptr;
    QComboBox* m_route = nullptr;
    QDoubleSpinBox* m_freqLo = nullptr;
    QDoubleSpinBox* m_freqHi = nullptr;
    QDoubleSpinBox* m_freqStep = nullptr;
    QDoubleSpinBox* m_attLo = nullptr;
    QDoubleSpinBox* m_attHi = nullptr;
    QDoubleSpinBox* m_span = nullptr;
    QLabel* m_planHint = nullptr;
    QLabel* m_benchMessage = nullptr;
    QLabel* m_detail = nullptr;
    QLabel* m_count = nullptr;
    QLabel* m_last = nullptr;
    QProgressBar* m_bar = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_confirm = nullptr;
    QPushButton* m_pdf = nullptr;
    QListWidget* m_archive = nullptr;
    QWidget* m_measureFields = nullptr;
    QString m_runId;
    QString m_waitPort;
    QString m_pdfPath;
};
