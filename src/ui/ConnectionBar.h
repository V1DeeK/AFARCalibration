#pragma once

#include <QFrame>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

/// Полоса соединений (UI-01): VNA, контроллер, температура, статус, выбор транспорта VNA.
class ConnectionBar final : public QFrame {
    Q_OBJECT

public:
    explicit ConnectionBar(QWidget* parent = nullptr);

    void setVnaInfo(const QString& model, const QString& address, bool connected);
    void setControllerInfo(const QString& iface, bool connected);
    void setTemperatureC(double temperature_c, bool valid);
    void setRunStatus(const QString& text, const QString& colorName);
    void setDiagnostic(const QString& text);
    void setThemeButtonText(const QString& text);
    /// UI-202: постоянная метка «имитатор» / «живой VNA».
    void setDataSourceText(const QString& text);

    void loadSettings();
    void saveSettings() const;

    [[nodiscard]] int vnaBackend() const;
    [[nodiscard]] QString vnaHost() const;
    [[nodiscard]] int vnaPort() const;
    [[nodiscard]] QString vnaComPort() const;
    [[nodiscard]] bool allowDirectAccess() const;

signals:
    void themeToggleRequested();
    void vnaSettingsChanged();
    void probeVnaRequested();

private slots:
    void onBackendChanged(int index);

private:
    void updateFieldsEnabled();

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
    QLineEdit* m_com = nullptr;
    QPushButton* m_probe = nullptr;
};
