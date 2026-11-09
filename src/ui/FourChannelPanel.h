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
class QSlider;
class QDoubleSpinBox;
class QSpinBox;
class QTimer;
class QToolButton;

/// «Настройка 4 каналов»: четыре канала одной SPI-цепочки.
/// В каждом SEND уходят все четыре слова. Слово выбранного канала берётся
/// с ползунка, остальные три — нули (−31.5 дБ), иначе открытый сосед держит
/// трассу анализатора около +30 дБ. Код считается записанным после OK платы.
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
    /// Снимок текущей трассы S2VNA без остановки непрерывного свипа.
    /// parameter: 0 S11, 1 S21, 2 S12, 3 S22.
    void onLiveTrace(bool ok, const QString& message, int parameter, const QVector<double>& freqGhz,
                     const QVector<double>& magDb);
    void onChannelGrid(bool ok, const QString& message, double stepMhz, int settleMs);
    void onChannelMarkers(bool ok, const QString& message, int stepIndex, double s11, bool s11Ok, bool s11Swr,
                          double s22, bool s22Ok, bool s22Swr, double s21, bool s21Ok, double s12, bool s12Ok,
                          double peak, bool peakOk, const QVector<double>& freqHz, const QVector<double>& gainDb,
                          const QVector<double>& phaseDeg);

signals:
    /// Нужен один полный снимок S11/S21/S12/S22 текущими настройками свипа.
    void channelSweepRequested();
    /// Сузить свип до середины текущей полосы.
    void channelGridRequested();
    /// Вернуть полосу и число точек анализатора после прохода.
    void channelBandRestoreRequested();
    /// Прочитать маркеры max/min/peak со всех четырёх графиков.
    void channelMarkersRequested(int stepIndex);

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
    void requestSend();
    void queueSend(bool force);
    void armSend();
    [[nodiscard]] bool sliderHeld() const;
    [[nodiscard]] bool codesDiffer() const;
    void lockUnselected();
    /// true — на экране уже другой код, его ещё надо отправить.
    bool commitConfirmed();
    void revertConfirmed(const QString& reason);

    void setPhaseCode(int code);
    void setAttenCode(int code);
    void setChannelPhase(int index, int code);
    void setChannelAtten(int index, int code);
    void setChannelOrder(int index, int lsbFirst);
    void render();
    void refreshPacket();
    /// Кадр на провод: выбранный канал как на экране, остальные — код 0.
    void outgoingCodes(std::array<int, 4>& phase, std::array<int, 4>& atten) const;
    void applyCfg();
    void readBoard();
    void applyBoardFrame(const QString& reply);
    void sendFrame();
    void sendFrame(const std::function<void(bool)>& after);
    void appendLog(const QString& line);

    void startSweep();
    void startAllChannels();
    void beginSweep(bool allChannels);
    void startRepeat();
    void stopMeasure(const QString& outcome);
    void askNextChannel();
    void applyBestAndSave();
    void restoreProduct();
    void continueMeasure();
    void applyMeasureStep();
    void onStepTimer();
    void finishMeasurePdf(bool openReport);
    void finishRepeatPdf();
    void finishSweepStep();
    void showReport(const QString& path);
    void chooseScreenshots();
    void openReportFiles();
    QString reportsRoot() const;
    QString latestReportDir() const;
    void setMeasureUi();
    void setEditEnabled();
    void rememberStep();
    void saveScreenshot(int index);

    Ppkh4Link* m_link = nullptr;
    afar::ppkh4::FrameFormat m_format;
    afar::ppkh4::LineConfig m_lines;
    std::array<int, 4> m_phase{};
    std::array<int, 4> m_atten{};
    std::array<int, 4> m_confirmedPhase{};
    std::array<int, 4> m_confirmedAtten{};
    std::array<int, 4> m_confirmedLsb{};
    std::array<int, 4> m_sentPhase{};
    std::array<int, 4> m_sentAtten{};
    std::array<int, 4> m_sentLsb{};
    std::array<int, 4> m_lsb{};
    bool m_sendInFlight = false;
    bool m_sendDirty = false;
    bool m_sendForce = false;
    bool m_userAdjusted = false;
    qint64 m_sendNotBefore = 0;
    QString m_lastSentHex;
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
    QPushButton* m_readBoard = nullptr;
    QLabel* m_boardLevel = nullptr;
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

    QWidget* m_phaseBox = nullptr;
    QWidget* m_attenBox = nullptr;
    QLabel* m_phVal = nullptr;
    QSlider* m_phRange = nullptr;
    QDoubleSpinBox* m_phDeg = nullptr;
    QSpinBox* m_phNum = nullptr;
    QLabel* m_atVal = nullptr;
    QSlider* m_atRange = nullptr;
    QDoubleSpinBox* m_atDb = nullptr;
    QSpinBox* m_atNum = nullptr;

    QPlainTextEdit* m_log = nullptr;

    enum class RunKind { None, Sweep, Repeat };
    enum class StepPhase { Idle, Settle, Collect };

    bool m_running = false;
    bool m_stopAsked = false;
    bool m_gridPending = false;
    bool m_allChannels = false;
    QString m_sweepNote;
    int m_sweepSettleMs = 1500;
    RunKind m_runKind = RunKind::None;
    StepPhase m_stepPhase = StepPhase::Idle;
    int m_stepIndex = 0;
    int m_pointBase = 0;
    int m_captureIndex = -1;
    int m_repeatTarget = 0;
    int m_repeatParameter = -1;
    bool m_repeatGot = false;
    QTimer* m_stepTimer = nullptr;
    QString m_runDir;
    QString m_runTitle;
    QString m_shownReport;
    std::array<int, 4> m_savedPhase{};
    std::array<int, 4> m_savedAtten{};
    QVector<ChannelStep> m_steps;
    QVector<int> m_shotSteps;
    QVector<ChannelPoint> m_points;
    QVector<double> m_repeatDb;
    QPushButton* m_measureStart = nullptr;
    QPushButton* m_measureAll = nullptr;
    QComboBox* m_product = nullptr;
    QPushButton* m_restoreProduct = nullptr;
    QLabel* m_configPath = nullptr;
    QPushButton* m_repeatStart = nullptr;
    QComboBox* m_repeatCount = nullptr;
    QPushButton* m_measureStop = nullptr;
    QPushButton* m_openReport = nullptr;
    QLabel* m_measureStatus = nullptr;
    QProgressBar* m_measureBar = nullptr;
};
