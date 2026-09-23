#include "ConnectionBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

ConnectionBar::ConnectionBar(QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setObjectName(QStringLiteral("ConnectionBar"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    auto* row = new QHBoxLayout();
    auto* title = new QLabel(QStringLiteral("AFAR RX Calibration Studio"), this);
    title->setStyleSheet(QStringLiteral("font-weight: 600;"));

    m_vna = new QLabel(QStringLiteral("C2220: нет связи"), this);
    m_controller = new QLabel(QStringLiteral("Контроллер: нет связи"), this);
    m_temperature = new QLabel(QStringLiteral("Температура: —"), this);
    m_status = new QLabel(QStringLiteral("Статус: простой"), this);

    row->addWidget(title);
    row->addStretch(1);
    row->addWidget(m_vna);
    row->addWidget(m_controller);
    row->addWidget(m_temperature);
    row->addWidget(m_status);
    m_theme = new QPushButton(QStringLiteral("Тема: светлая"), this);
    m_theme->setObjectName(QStringLiteral("btnTheme"));
    connect(m_theme, &QPushButton::clicked, this, &ConnectionBar::themeToggleRequested);
    row->addWidget(m_theme);
    root->addLayout(row);

    m_diagnostic = new QLabel(this);
    m_diagnostic->setWordWrap(true);
    m_diagnostic->setVisible(false);
    m_diagnostic->setStyleSheet(
        QStringLiteral("background:#fff3cd; color:#664d03; padding:4px 6px; border-radius:3px;"));
    root->addWidget(m_diagnostic);
}

void ConnectionBar::setVnaInfo(const QString& model, const QString& address, bool connected)
{
    if (connected) {
        m_vna->setText(QStringLiteral("C2220: %1 @ %2").arg(model, address));
        m_vna->setStyleSheet(QStringLiteral("color:#0a7a2f;"));
    } else {
        m_vna->setText(QStringLiteral("C2220: нет связи (%1 @ %2)").arg(model, address));
        m_vna->setStyleSheet(QStringLiteral("color:#8a1f11;"));
    }
}

void ConnectionBar::setControllerInfo(const QString& iface, bool connected)
{
    if (connected) {
        m_controller->setText(QStringLiteral("Контроллер: %1").arg(iface));
        m_controller->setStyleSheet(QStringLiteral("color:#0a7a2f;"));
    } else {
        m_controller->setText(QStringLiteral("Контроллер: нет связи (%1)").arg(iface));
        m_controller->setStyleSheet(QStringLiteral("color:#8a1f11;"));
    }
}

void ConnectionBar::setTemperatureC(double temperature_c, bool valid)
{
    if (!valid) {
        m_temperature->setText(QStringLiteral("Температура: —"));
        return;
    }
    m_temperature->setText(
        QStringLiteral("Температура: %1 °C").arg(temperature_c, 0, 'f', 1));
}

void ConnectionBar::setRunStatus(const QString& text, const QString& colorName)
{
    m_status->setText(QStringLiteral("Статус: %1").arg(text));
    m_status->setStyleSheet(QStringLiteral("color:%1; font-weight:600;").arg(colorName));
}

void ConnectionBar::setDiagnostic(const QString& text)
{
    const bool show = !text.isEmpty();
    m_diagnostic->setVisible(show);
    m_diagnostic->setText(show ? QStringLiteral("Диагностика: %1").arg(text) : QString());
}

void ConnectionBar::setThemeButtonText(const QString& text)
{
    m_theme->setText(text);
}
