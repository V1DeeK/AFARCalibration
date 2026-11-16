#pragma once

#include "video/VideoProto.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;
class QThread;
class StatTile;
class VideoSession;
class VideoCapturePane;
class RssiChart;

/// Вкладка «Видеоприёмник»: модуль Video_receiver_49_60 (узел VIDEO, Modbus RTU по USB-RS485).
/// Перенос веб-интерфейса tools/board_tester.html репозитория V1DeeK/Video_receiver:
/// монитор и настройка приёма, видео с карты захвата, проверка платы, прошивка, журнал обмена.
class VideoReceiverTab final : public QWidget {
    Q_OBJECT

public:
    explicit VideoReceiverTab(QWidget* parent = nullptr);
    ~VideoReceiverTab() override;

private:
    struct TestRow {
        QString name, result, text;
    };
    struct TestReport {
        QString time, serial, operatorName, verdict, signal;
        video::Status identity;
        QList<TestRow> rows;
    };

    QWidget* buildConnection();
    QWidget* buildMonitor();
    QWidget* buildTest();
    QWidget* buildFirmware();
    QWidget* buildJournal();
    QWidget* buildStatus();
    void refreshPorts();
    void toggleConnection();
    void setBusy(bool busy);
    void updateControls();
    void applyNow(double mhz);
    void showStatus(const video::Status& s);
    void showOp(const QString& op, bool ok, const QString& text);
    void chooseFirmware();
    [[nodiscard]] QString reportText(const TestReport& r) const;
    void saveReport();
    void saveHistory();
    [[nodiscard]] video::ApplyRequest requestFromUi(double mhz) const;

    QThread* m_thread = nullptr;
    VideoSession* m_session = nullptr;
    bool m_open = false;
    bool m_busy = false;
    QByteArray m_image;
    QList<TestReport> m_history;
    TestReport m_current;

    QComboBox* m_port = nullptr;
    QSpinBox* m_addr = nullptr;
    QPushButton* m_connect = nullptr;
    QLabel* m_link = nullptr;
    QTabWidget* m_pages = nullptr;

    QDoubleSpinBox* m_freq = nullptr;
    QComboBox* m_preset = nullptr;
    QComboBox* m_profile = nullptr;
    QCheckBox* m_rfEnable = nullptr;
    QComboBox* m_power = nullptr;
    QComboBox* m_agc = nullptr;
    QDoubleSpinBox* m_attenDb = nullptr;
    QSpinBox* m_target = nullptr;
    QCheckBox* m_sendExt = nullptr;
    QComboBox* m_bandMode = nullptr;
    QComboBox* m_cp = nullptr;
    QPushButton* m_cpWrite = nullptr;
    QLabel* m_cpResult = nullptr;
    QPushButton* m_apply = nullptr;
    QPushButton* m_safe = nullptr;
    QPushButton* m_clear = nullptr;
    QLabel* m_applyResult = nullptr;
    QCheckBox* m_rtcAudio = nullptr;
    QCheckBox* m_rtcRf = nullptr;
    QPushButton* m_rtcWrite = nullptr;
    QLabel* m_rtcResult = nullptr;
    QPushButton* m_regsRead = nullptr;
    QLabel* m_regsInfo = nullptr;
    QTableWidget* m_regs = nullptr;
    QComboBox* m_osdStd = nullptr;
    QSpinBox* m_osdRow = nullptr;
    QSpinBox* m_osdCol = nullptr;
    QCheckBox* m_osdEnable = nullptr;
    QLineEdit* m_osdText = nullptr;
    QComboBox* m_osdFont = nullptr;
    QCheckBox* m_osdInv = nullptr;
    QCheckBox* m_osdBlink = nullptr;
    QCheckBox* m_osdBg = nullptr;
    QPushButton* m_osdWrite = nullptr;
    QPushButton* m_osdClear = nullptr;
    QLabel* m_osdResult = nullptr;
    QLabel* m_osdCodes = nullptr;
    RssiChart* m_chart = nullptr;
    QPushButton* m_scan = nullptr;
    QLabel* m_scanInfo = nullptr;
    QTableWidget* m_scanTable = nullptr;
    VideoCapturePane* m_capture = nullptr;

    QLineEdit* m_serial = nullptr;
    QLineEdit* m_operator = nullptr;
    QCheckBox* m_haveSignal = nullptr;
    QDoubleSpinBox* m_sigFreq = nullptr;
    QCheckBox* m_haveVideo = nullptr;
    QPushButton* m_testStart = nullptr;
    QPushButton* m_testStop = nullptr;
    QPushButton* m_reportSave = nullptr;
    QLabel* m_verdict = nullptr;
    QTableWidget* m_testTable = nullptr;
    QTableWidget* m_histTable = nullptr;

    QLineEdit* m_fwPath = nullptr;
    QLineEdit* m_fwVersion = nullptr;
    QLabel* m_fwInfo = nullptr;
    QPushButton* m_flash = nullptr;
    QPushButton* m_recover = nullptr;
    QPushButton* m_fwStop = nullptr;
    QProgressBar* m_fwBar = nullptr;
    QLabel* m_fwStatus = nullptr;

    QCheckBox* m_logPoll = nullptr;
    QPlainTextEdit* m_log = nullptr;

    QLabel* m_state = nullptr;
    QLabel* m_error = nullptr;
    QLabel* m_flags = nullptr;
    QLabel* m_stats = nullptr;
    QCheckBox* m_autoPoll = nullptr;
    QList<QLabel*> m_values;
};
