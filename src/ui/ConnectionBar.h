#pragma once

#include <QFrame>

class QLabel;
class QPushButton;

/// Полоса соединений (UI-01 / UI-003): C2220, контроллер, температура, статус текстом и цветом.
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

signals:
    void themeToggleRequested();

private:
    QLabel* m_vna = nullptr;
    QLabel* m_controller = nullptr;
    QLabel* m_temperature = nullptr;
    QLabel* m_status = nullptr;
    QLabel* m_diagnostic = nullptr;
    QPushButton* m_theme = nullptr;
};
