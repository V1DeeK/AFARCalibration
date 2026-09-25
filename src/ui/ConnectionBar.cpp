#include "ConnectionBar.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
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
    m_controller = new QLabel(
        QStringLiteral("Контроллер серии: ИМИТАТОР (для S-параметров не нужен)"), this);
    m_controller->setObjectName(QStringLiteral("controllerStatus"));
    m_filterReady = new QLabel(QStringLiteral("S-параметры: нужна связь VNA"), this);
    m_filterReady->setObjectName(QStringLiteral("filterReadyStatus"));
    m_temperature = new QLabel(QStringLiteral("Температура: —"), this);
    m_status = new QLabel(QStringLiteral("Статус: простой"), this);
    m_source = new QLabel(QStringLiteral("Источник: имитатор (не метрология стенда)"), this);
    m_source->setObjectName(QStringLiteral("dataSourceBadge"));
    m_source->setStyleSheet(
        QStringLiteral("color:#664d03; font-weight:600; padding:1px 4px;"));

    row->addWidget(title);
    row->addStretch(1);
    row->addWidget(m_source);
    row->addWidget(m_filterReady);
    row->addWidget(m_vna);
    row->addWidget(m_controller);
    row->addWidget(m_temperature);
    row->addWidget(m_status);
    m_theme = new QPushButton(QStringLiteral("Тема: светлая"), this);
    m_theme->setObjectName(QStringLiteral("btnTheme"));
    connect(m_theme, &QPushButton::clicked, this, &ConnectionBar::themeToggleRequested);
    row->addWidget(m_theme);
    root->addLayout(row);

    auto* cfg = new QHBoxLayout();
    cfg->addWidget(new QLabel(QStringLiteral("VNA:"), this));
    m_backend = new QComboBox(this);
    m_backend->addItem(QStringLiteral("Имитатор"), 0);
    m_backend->addItem(QStringLiteral("S2VNA Socket"), 1);
    m_backend->addItem(QStringLiteral("S2VNA COM"), 2);
    cfg->addWidget(m_backend);
    cfg->addWidget(new QLabel(QStringLiteral("Host"), this));
    m_host = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_host->setMaximumWidth(140);
    cfg->addWidget(m_host);
    cfg->addWidget(new QLabel(QStringLiteral("Port"), this));
    m_port = new QSpinBox(this);
    m_port->setRange(1, 65535);
    m_port->setValue(5025);
    cfg->addWidget(m_port);
    cfg->addWidget(new QLabel(QStringLiteral("COM"), this));
    m_com = new QLineEdit(QStringLiteral("COM3"), this);
    m_com->setMaximumWidth(80);
    cfg->addWidget(m_com);
    m_probe = new QPushButton(QStringLiteral("Проверить связь"), this);
    m_probe->setObjectName(QStringLiteral("btnPrimary"));
    cfg->addWidget(m_probe);
    cfg->addStretch(1);
    root->addLayout(cfg);

    m_diagnostic = new QLabel(this);
    m_diagnostic->setWordWrap(true);
    m_diagnostic->setVisible(false);
    m_diagnostic->setStyleSheet(
        QStringLiteral("background:#fff3cd; color:#664d03; padding:4px 6px; border-radius:3px;"));
    root->addWidget(m_diagnostic);

    connect(m_backend, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConnectionBar::onBackendChanged);
    connect(m_probe, &QPushButton::clicked, this, [this]() {
        saveSettings();
        emit vnaSettingsChanged();
        emit probeVnaRequested();
    });
    connect(m_host, &QLineEdit::editingFinished, this, [this]() {
        saveSettings();
        emit vnaSettingsChanged();
    });
    connect(m_port, &QSpinBox::editingFinished, this, [this]() {
        saveSettings();
        emit vnaSettingsChanged();
    });
    connect(m_com, &QLineEdit::editingFinished, this, [this]() {
        saveSettings();
        emit vnaSettingsChanged();
    });

    loadSettings();
    updateFieldsEnabled();
}

void ConnectionBar::onBackendChanged(int)
{
    updateFieldsEnabled();
    saveSettings();
    emit vnaSettingsChanged();
}

void ConnectionBar::updateFieldsEnabled()
{
    const int b = vnaBackend();
    m_host->setEnabled(b == 1);
    m_port->setEnabled(b == 1);
    m_com->setEnabled(b == 2);
}

void ConnectionBar::loadSettings()
{
    QSettings s;
    const int backend = s.value(QStringLiteral("vna/backend"), 0).toInt();
    m_backend->setCurrentIndex(qBound(0, backend, 2));
    m_host->setText(s.value(QStringLiteral("vna/host"), QStringLiteral("127.0.0.1")).toString());
    m_port->setValue(s.value(QStringLiteral("vna/port"), 5025).toInt());
    m_com->setText(s.value(QStringLiteral("vna/com"), QStringLiteral("COM3")).toString());
    updateFieldsEnabled();
}

void ConnectionBar::saveSettings() const
{
    QSettings s;
    s.setValue(QStringLiteral("vna/backend"), vnaBackend());
    s.setValue(QStringLiteral("vna/host"), vnaHost());
    s.setValue(QStringLiteral("vna/port"), vnaPort());
    s.setValue(QStringLiteral("vna/com"), vnaComPort());
}

int ConnectionBar::vnaBackend() const
{
    return m_backend->currentData().toInt();
}

QString ConnectionBar::vnaHost() const
{
    return m_host->text().trimmed();
}

int ConnectionBar::vnaPort() const
{
    return m_port->value();
}

QString ConnectionBar::vnaComPort() const
{
    return m_com->text().trimmed();
}

bool ConnectionBar::allowDirectAccess() const
{
    return false;
}

void ConnectionBar::setVnaInfo(const QString& model, const QString& address, bool connected)
{
    if (connected) {
        m_vna->setText(QStringLiteral("C2220: %1 @ %2").arg(model, address));
        m_vna->setStyleSheet(QStringLiteral("color:#0a7a2f;"));
        m_filterReady->setText(QStringLiteral("S-параметры: ГОТОВО"));
        m_filterReady->setStyleSheet(QStringLiteral("color:#0a7a2f; font-weight:700;"));
    } else {
        m_vna->setText(QStringLiteral("C2220: нет связи (%1 @ %2)").arg(model, address));
        m_vna->setStyleSheet(QStringLiteral("color:#8a1f11;"));
        m_filterReady->setText(QStringLiteral("S-параметры: нужна связь VNA"));
        m_filterReady->setStyleSheet(QStringLiteral("color:#8a1f11; font-weight:700;"));
    }
}

void ConnectionBar::setControllerInfo(const QString& iface, bool connected)
{
    const bool simulator = iface.contains(QStringLiteral("sim"), Qt::CaseInsensitive)
        || iface.contains(QStringLiteral("имитатор"), Qt::CaseInsensitive);
    if (simulator) {
        m_controller->setText(
            QStringLiteral("Контроллер серии: ИМИТАТОР (для S-параметров не нужен)"));
        m_controller->setStyleSheet(QStringLiteral("color:#9a6700;"));
        return;
    }
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

void ConnectionBar::setDataSourceText(const QString& text)
{
    m_source->setText(text);
}
