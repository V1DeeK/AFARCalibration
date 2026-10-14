#pragma once

#include <QElapsedTimer>
#include <QWidget>

class PsExchangeView;
class PsSession;
class StressTestPanel;
class UdpFloodGenerator;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

/// Вкладка «Тестовый прогон»: всё для проверки обмена с платой — стресс-тест
/// команд по TCP (из «Теста обмена» РМО), проверка потока UDP-телеметрии и
/// состояния узлов (ПЛИС, RS-485 и др.), сырые байты в линию RS-485 и журнал.
class TestRunTab final : public QWidget {
    Q_OBJECT

public:
    explicit TestRunTab(PsSession* session, QWidget* parent = nullptr);

    [[nodiscard]] StressTestPanel* stress() const { return m_stress; }
    [[nodiscard]] UdpFloodGenerator* flood() const { return m_flood; }
    /// Проверка потока телеметрии на seconds секунд.
    void startUdpCheck(int seconds);
    [[nodiscard]] bool udpCheckRunning() const;

private:
    QWidget* buildStressPage();
    QWidget* buildUdpPage();
    QWidget* buildRs485Page();
    void refreshUdp();
    void finishUdpCheck();
    void refreshEnabled();

    PsSession* m_session;
    StressTestPanel* m_stress = nullptr;
    UdpFloodGenerator* m_flood = nullptr;
    QCheckBox* m_floodOn = nullptr;
    QSpinBox* m_floodRate = nullptr;
    QLabel* m_floodState = nullptr;

    QLabel* m_udpCounters = nullptr;
    QLabel* m_udpVerdict = nullptr;
    QSpinBox* m_udpSeconds = nullptr;
    QPushButton* m_udpStart = nullptr;
    QTableWidget* m_devices = nullptr;
    QTimer* m_udpTimer = nullptr;
    QElapsedTimer m_udpClock;
    quint64 m_udpStartReceived = 0;
    quint64 m_udpStartLost = 0;
    quint64 m_udpStartRejected = 0;

    QLineEdit* m_rawHex = nullptr;
    QPushButton* m_rawSend = nullptr;
    QLabel* m_rawResult = nullptr;
};
