#pragma once

#include <QFrame>
#include <QStringList>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QTabWidget;

/// Полоса соединений (UI-01): VNA, контроллер, температура, статус, выбор транспорта VNA.
class ConnectionBar final : public QFrame {
    Q_OBJECT

public:
    /// Третья страница общей области рядом с подключением и контроллером.
    void setMiddleWidget(QWidget* widget);
    void setSweepWidget(QWidget* widget);
    /// «Настройка 1 канала» (плата ППКХ4) — сразу после «Настройки частоты».
    void setSingleChannelWidget(QWidget* widget);
    void showSweepSettings();
    /// Очередь ошибок SCPI; MainWindow переносит её в «Тех. состояние».
    [[nodiscard]] QWidget* scpiPanel() const { return m_scpiPanel; }
    explicit ConnectionBar(QWidget* parent = nullptr);

    void setVnaInfo(const QString& model,
                    const QString& address,
                    bool connected,
                    const QString& serial = {},
                    const QString& firmware = {});
    /// CTRL: OK / нет связи / stub — по iface и connected.
    void setControllerInfo(const QString& iface, bool connected);
    void setTemperatureC(double temperature_c, bool valid);
    void setRunStatus(const QString& text, const QString& colorName);
    void setDiagnostic(const QString& text);
    void setThemeButtonText(const QString& text);
    /// UI-202: постоянная метка «имитатор» / «живой VNA».
    void setDataSourceText(const QString& text);

    /// UI-ERR-001: дописать строки очереди SCPI (немодально). Clear — только UI-буфер.
    void appendScpiErrors(const QStringList& entries);
    void clearScpiErrorQueue();

    /// После подготовки серии настройки VNA неизменяемы до терминального состояния.
    void setVnaSettingsLocked(bool locked);
    void setVnaConnectionActive(bool active);

    void loadSettings();
    void saveSettings() const;

    [[nodiscard]] int vnaBackend() const;
    [[nodiscard]] QString vnaHost() const;
    [[nodiscard]] int vnaPort() const;
    [[nodiscard]] bool allowDirectAccess() const;
    [[nodiscard]] int vnaConnectTimeoutMs() const;
    [[nodiscard]] int vnaSweepTimeoutMs() const;
    [[nodiscard]] int vnaMeasureRetries() const;

    /// DUT-UI-001: 0=DutSimulator, 1=Stub (т.14), 2=боевой (disabled до т.14).
    [[nodiscard]] int controllerBackend() const;
    [[nodiscard]] QString dutHost() const;
    [[nodiscard]] int dutPort() const;
    [[nodiscard]] QString dutComPort() const;

signals:
    void themeToggleRequested();
    void vnaSettingsChanged();
    void controllerSettingsChanged();
    void vnaConnectionToggleRequested(bool enable);
    void simulateScpiErrorRequested();

private slots:
    void onBackendChanged(int index);
    void onControllerBackendChanged(int index);

private:
    void updateFieldsEnabled();
    void updateDutEndpointEnabled();
    void updateScpiQueueVisibility();
    void emitTimeoutsChanged();
    void disableCombatControllerItem();

    QLabel* m_vna = nullptr;
    QLabel* m_controller = nullptr;
    QLabel* m_filterReady = nullptr;
    QLabel* m_temperature = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_source = nullptr;
    QLabel* m_diagnostic = nullptr;
    QPushButton* m_theme = nullptr;
    QComboBox* m_backend = nullptr;
    QLineEdit* m_host = nullptr;
    QSpinBox* m_port = nullptr;
    QPushButton* m_probe = nullptr;

    QComboBox* m_ctrlBackend = nullptr;
    QLineEdit* m_dutHost = nullptr;
    QSpinBox* m_dutPort = nullptr;
    QLineEdit* m_dutCom = nullptr;
    int m_lastCtrlBackendIndex{0};

    QWidget* m_scpiPanel = nullptr;
    QListWidget* m_scpiErrors = nullptr;
    QPushButton* m_scpiClear = nullptr;
    QPushButton* m_scpiSimulate = nullptr;

    QTabWidget* m_panels = nullptr;
    QSpinBox* m_connectMs = nullptr;
    QSpinBox* m_sweepMs = nullptr;
    QSpinBox* m_retries = nullptr;
    bool m_timeoutsLocked = false;
    bool m_vnaConnectionActive = false;
};
