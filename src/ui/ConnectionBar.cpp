#include "ConnectionBar.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString formatScpiErrorLine(const QString& raw)
{
    const QString line = raw.trimmed();
    if (line.isEmpty()) {
        return line;
    }
    // Опционально: "CMD · code, text" из журнала; иначе SYST:ERR? — "code, text".
    QString command;
    QString payload = line;
    const int sep = line.indexOf(QStringLiteral(" · "));
    if (sep > 0) {
        command = line.left(sep).trimmed();
        payload = line.mid(sep + 3).trimmed();
    }
    const int comma = payload.indexOf(QLatin1Char(','));
    QString code;
    QString text;
    if (comma >= 0) {
        code = payload.left(comma).trimmed();
        text = payload.mid(comma + 1).trimmed();
        if (text.size() >= 2 && text.startsWith(QLatin1Char('"')) && text.endsWith(QLatin1Char('"'))) {
            text = text.mid(1, text.size() - 2);
        }
    } else {
        text = payload;
    }
    if (!command.isEmpty() && !code.isEmpty()) {
        return QStringLiteral("%1 · код %2 · %3").arg(command, code, text);
    }
    if (!code.isEmpty()) {
        return QStringLiteral("код %1 · %2").arg(code, text);
    }
    return text.isEmpty() ? line : text;
}

}  // namespace

ConnectionBar::ConnectionBar(QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setObjectName(QStringLiteral("ConnectionBar"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(4);

    auto* row = new QGridLayout();
    m_vna = new QLabel(QStringLiteral("C2220: нет связи"), this);
    m_controller = new QLabel(
        QStringLiteral("CTRL: OK — ИМИТАТОР (для S-параметров не нужен)"), this);
    m_controller->setObjectName(QStringLiteral("controllerStatus"));
    m_filterReady = new QLabel(QStringLiteral("S-параметры: нужна связь VNA"), this);
    m_filterReady->setObjectName(QStringLiteral("filterReadyStatus"));
    m_temperature = new QLabel(QStringLiteral("Температура: —"), this);
    m_status = new QLabel(QStringLiteral("Статус: простой"), this);
    m_source = new QLabel(QStringLiteral("Источник: имитатор (не метрология стенда)"), this);
    m_source->setObjectName(QStringLiteral("dataSourceBadge"));
    m_source->setStyleSheet(
        QStringLiteral("color:#664d03; font-weight:600; padding:1px 4px;"));

    for (auto* label : {m_source, m_filterReady, m_vna, m_controller, m_temperature, m_status}) {
        label->setWordWrap(true);
    }
    row->addWidget(m_source, 0, 0);
    row->addWidget(m_filterReady, 0, 1);
    row->addWidget(m_vna, 0, 2);
    row->addWidget(m_controller, 1, 0);
    row->addWidget(m_temperature, 1, 1);
    row->addWidget(m_status, 1, 2);
    auto* settingsToggle = new QToolButton(this);
    settingsToggle->setObjectName(QStringLiteral("connectionSettingsToggle"));
    settingsToggle->setText(QStringLiteral("Настройки подключения"));
    settingsToggle->setCheckable(true);
    settingsToggle->setArrowType(Qt::RightArrow);
    settingsToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    row->addWidget(settingsToggle, 0, 3);
    m_theme = new QPushButton(QStringLiteral("Тема: светлая"), this);
    m_theme->setObjectName(QStringLiteral("btnTheme"));
    connect(m_theme, &QPushButton::clicked, this, &ConnectionBar::themeToggleRequested);
    row->addWidget(m_theme, 1, 3);
    row->setColumnStretch(0, 2);
    row->setColumnStretch(1, 1);
    row->setColumnStretch(2, 1);
    root->addLayout(row);

    auto* settingsBody = new QWidget(this);
    settingsBody->setObjectName(QStringLiteral("connectionSettingsBody"));
    auto* settingsLayout = new QVBoxLayout(settingsBody);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(2);
    settingsBody->setVisible(false);
    connect(settingsToggle, &QToolButton::toggled, this,
            [settingsToggle, settingsBody](bool expanded) {
                settingsToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
                settingsBody->setVisible(expanded);
            });

    auto* cfg = new QHBoxLayout();
    cfg->addWidget(new QLabel(QStringLiteral("VNA:"), this));
    m_backend = new QComboBox(this);
    m_backend->setObjectName(QStringLiteral("vnaBackend"));
    m_backend->addItem(QStringLiteral("Связанный демостенд (VNA + DUT)"), 0);
    // QSettings vna/backend = 0/1/2/3 (userData); подписи меняются, ключи нет.
    // Alias «S2VNA …» в UserRole+1 — для поиска/подсказок операторам со старым именем.
    m_backend->addItem(QStringLiteral("C2220 Socket (SCPI)"), 1);
    m_backend->setItemData(1, QStringLiteral("S2VNA Socket"), Qt::UserRole + 1);
    m_backend->setItemData(
        1,
        QStringLiteral(
            "Живой C1220/C2220 по TCP. S2VNA запускается скрыто автоматически."),
        Qt::ToolTipRole);
    m_backend->addItem(QStringLiteral("C2220 COM (SCPI)"), 2);
    m_backend->setItemData(2, QStringLiteral("S2VNA COM"), Qt::UserRole + 1);
    m_backend->setItemData(
        2,
        QStringLiteral(
            "Живой C2220 по COM. Нужен запущенный SCPI-сервер S2VNA (удалённый COM)."),
        Qt::ToolTipRole);
    m_backend->addItem(QStringLiteral("S2VNA Demo C2220 (Socket)"), 3);
    m_backend->setItemData(3, QStringLiteral("S2VNA Demo Mode"), Qt::UserRole + 1);
    m_backend->setItemData(
        3,
        QStringLiteral(
            "Демо-измерения S2VNA без прибора. Графики и отчёты не являются метрологией."),
        Qt::ToolTipRole);
    m_backend->setToolTip(
        QStringLiteral("S2VNA = SCPI-сервер; калибровка и измерения — из AFAR."));
    cfg->addWidget(m_backend);
    cfg->addWidget(new QLabel(QStringLiteral("Host"), this));
    m_host = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_host->setObjectName(QStringLiteral("vnaHost"));
    m_host->setMaximumWidth(140);
    cfg->addWidget(m_host);
    cfg->addWidget(new QLabel(QStringLiteral("Port"), this));
    m_port = new QSpinBox(this);
    m_port->setObjectName(QStringLiteral("vnaPort"));
    m_port->setRange(1, 65535);
    m_port->setValue(5025);
    cfg->addWidget(m_port);
    cfg->addWidget(new QLabel(QStringLiteral("COM"), this));
    m_com = new QLineEdit(QStringLiteral("COM3"), this);
    m_com->setObjectName(QStringLiteral("vnaComPort"));
    m_com->setMaximumWidth(80);
    cfg->addWidget(m_com);
    m_probe = new QPushButton(QStringLiteral("Проверить связь"), this);
    m_probe->setObjectName(QStringLiteral("btnPrimary"));
    cfg->addWidget(m_probe);
    cfg->addStretch(1);
    settingsLayout->addLayout(cfg);

    // Контроллер изделия отсутствует: доступен только честно обозначенный деморежим.
    auto* dutCfg = new QHBoxLayout();
    dutCfg->addWidget(new QLabel(QStringLiteral("CTRL:"), this));
    m_ctrlBackend = new QComboBox(this);
    m_ctrlBackend->setObjectName(QStringLiteral("controllerBackend"));
    m_ctrlBackend->addItem(QStringLiteral("Демо-контроллер (DutSimulator)"), 0);
    m_ctrlBackend->addItem(QStringLiteral("Реальный контроллер — отсутствует"), 2);
    dutCfg->addWidget(m_ctrlBackend);
    dutCfg->addWidget(new QLabel(QStringLiteral("Host"), this));
    m_dutHost = new QLineEdit(QStringLiteral("192.168.0.10"), this);
    m_dutHost->setMaximumWidth(140);
    m_dutHost->setToolTip(QStringLiteral("нужен протокол т.14"));
    dutCfg->addWidget(m_dutHost);
    dutCfg->addWidget(new QLabel(QStringLiteral("Port"), this));
    m_dutPort = new QSpinBox(this);
    m_dutPort->setRange(1, 65535);
    m_dutPort->setValue(4001);
    m_dutPort->setToolTip(QStringLiteral("нужен протокол т.14"));
    dutCfg->addWidget(m_dutPort);
    dutCfg->addWidget(new QLabel(QStringLiteral("COM"), this));
    m_dutCom = new QLineEdit(QStringLiteral("COM4"), this);
    m_dutCom->setMaximumWidth(80);
    m_dutCom->setToolTip(QStringLiteral("нужен протокол т.14"));
    dutCfg->addWidget(m_dutCom);
    dutCfg->addStretch(1);
    settingsLayout->addLayout(dutCfg);
    root->addWidget(settingsBody);
    disableCombatControllerItem();

    m_diagnostic = new QLabel(this);
    m_diagnostic->setWordWrap(true);
    m_diagnostic->setVisible(false);
    m_diagnostic->setStyleSheet(
        QStringLiteral("background:#fff3cd; color:#664d03; padding:4px 6px; border-radius:3px;"));
    root->addWidget(m_diagnostic);

    // UI-ERR-001: немодальная очередь SCPI (не QMessageBox).
    m_scpiPanel = new QWidget(this);
    auto* scpiLay = new QVBoxLayout(m_scpiPanel);
    scpiLay->setContentsMargins(0, 2, 0, 0);
    scpiLay->setSpacing(2);
    auto* scpiHead = new QHBoxLayout();
    auto* scpiTitle = new QLabel(QStringLiteral("Очередь ошибок SCPI"), m_scpiPanel);
    scpiTitle->setStyleSheet(QStringLiteral("font-weight:600; color:#8a1f11;"));
    scpiHead->addWidget(scpiTitle);
    scpiHead->addStretch(1);
    m_scpiSimulate = new QPushButton(QStringLiteral("Симулировать ошибку SCPI"), m_scpiPanel);
    m_scpiSimulate->setVisible(false);
    scpiHead->addWidget(m_scpiSimulate);
    m_scpiClear = new QPushButton(QStringLiteral("Очистить"), m_scpiPanel);
    scpiHead->addWidget(m_scpiClear);
    scpiLay->addLayout(scpiHead);
    m_scpiErrors = new QListWidget(m_scpiPanel);
    m_scpiErrors->setMaximumHeight(88);
    m_scpiErrors->setObjectName(QStringLiteral("scpiErrorQueue"));
    m_scpiErrors->setStyleSheet(
        QStringLiteral("QListWidget { background:#fdecea; border:1px solid #f5c2c0; "
                       "border-radius:4px; }"));
    scpiLay->addWidget(m_scpiErrors);
    m_scpiPanel->setVisible(false);
    root->addWidget(m_scpiPanel);

    // UI-TO-001: инженерные тайм-ауты → QSettings → C2220Vna::Profile.
    m_timeoutsBox = new QGroupBox(QStringLiteral("Инженер: тайм-ауты VNA"), this);
    m_timeoutsBox->setObjectName(QStringLiteral("vnaTimeoutsBox"));
    m_timeoutsBox->setCheckable(true);
    m_timeoutsBox->setChecked(false);
    m_timeoutsBox->setFlat(true);
    auto* timeoutLayout = new QVBoxLayout(m_timeoutsBox);
    timeoutLayout->setContentsMargins(8, 4, 8, 4);
    auto* timeoutBody = new QWidget(m_timeoutsBox);
    timeoutBody->setObjectName(QStringLiteral("vnaTimeoutsBody"));
    auto* toForm = new QFormLayout(timeoutBody);
    toForm->setContentsMargins(8, 8, 8, 4);
    toForm->setHorizontalSpacing(10);
    m_connectMs = new QSpinBox(m_timeoutsBox);
    m_connectMs->setRange(100, 120000);
    m_connectMs->setSingleStep(100);
    m_connectMs->setSuffix(QStringLiteral(" мс"));
    m_connectMs->setValue(3000);
    m_sweepMs = new QSpinBox(m_timeoutsBox);
    m_sweepMs->setRange(1000, 600000);
    m_sweepMs->setSingleStep(1000);
    m_sweepMs->setSuffix(QStringLiteral(" мс"));
    m_sweepMs->setValue(30000);
    m_retries = new QSpinBox(m_timeoutsBox);
    m_retries->setRange(0, 10);
    m_retries->setValue(2);
    toForm->addRow(QStringLiteral("connect_ms"), m_connectMs);
    toForm->addRow(QStringLiteral("sweep_ms"), m_sweepMs);
    toForm->addRow(QStringLiteral("retries"), m_retries);
    timeoutLayout->addWidget(timeoutBody);
    timeoutBody->setVisible(false);
    root->addWidget(m_timeoutsBox);

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
    connect(m_scpiClear, &QPushButton::clicked, this, &ConnectionBar::clearScpiErrorQueue);
    connect(m_scpiSimulate, &QPushButton::clicked, this,
            &ConnectionBar::simulateScpiErrorRequested);
    connect(m_timeoutsBox, &QGroupBox::toggled, timeoutBody, &QWidget::setVisible);
    connect(m_connectMs, &QSpinBox::editingFinished, this, &ConnectionBar::emitTimeoutsChanged);
    connect(m_sweepMs, &QSpinBox::editingFinished, this, &ConnectionBar::emitTimeoutsChanged);
    connect(m_retries, &QSpinBox::editingFinished, this, &ConnectionBar::emitTimeoutsChanged);
    connect(m_ctrlBackend, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConnectionBar::onControllerBackendChanged);

    loadSettings();
    updateFieldsEnabled();
    updateDutEndpointEnabled();
}

void ConnectionBar::onBackendChanged(int)
{
    updateFieldsEnabled();
    saveSettings();
    emit vnaSettingsChanged();
}

void ConnectionBar::onControllerBackendChanged(int)
{
    const int mode = controllerBackend();
    if (mode == 2) {
        QMessageBox::information(
            this, QStringLiteral("Контроллер изделия отсутствует"),
            QStringLiteral(
                "Реальная автоматическая калибровка 64×64 недоступна.\n"
                "Используйте деморежим или подключите поддерживаемый контроллер."));
        m_ctrlBackend->blockSignals(true);
        m_ctrlBackend->setCurrentIndex(m_lastCtrlBackendIndex);
        m_ctrlBackend->blockSignals(false);
        disableCombatControllerItem();
        return;
    }
    m_lastCtrlBackendIndex = m_ctrlBackend->currentIndex();
    updateDutEndpointEnabled();
    saveSettings();
    emit controllerSettingsChanged();
}

void ConnectionBar::disableCombatControllerItem()
{
    auto* model = qobject_cast<QStandardItemModel*>(m_ctrlBackend->model());
    const int row = m_ctrlBackend->findData(2);
    if (!model || row < 0) {
        return;
    }
    QStandardItem* item = model->item(row);
    if (!item) {
        return;
    }
    item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
    item->setToolTip(QStringLiteral(
        "Контроллер изделия и его цифровой интерфейс отсутствуют"));
}

void ConnectionBar::updateDutEndpointEnabled()
{
    // Endpoint для будущего COM/TCP: всегда disabled до т. 14.
    m_dutHost->setEnabled(false);
    m_dutPort->setEnabled(false);
    m_dutCom->setEnabled(false);
    const QString tip = QStringLiteral(
        "Контроллер изделия и его цифровой интерфейс отсутствуют");
    m_dutHost->setToolTip(tip);
    m_dutPort->setToolTip(tip);
    m_dutCom->setToolTip(tip);
}

void ConnectionBar::updateFieldsEnabled()
{
    const int b = vnaBackend();
    const bool settingsEnabled = !m_timeoutsLocked;
    m_backend->setEnabled(settingsEnabled);
    m_host->setEnabled(settingsEnabled && (b == 1 || b == 3));
    m_port->setEnabled(settingsEnabled && (b == 1 || b == 3));
    m_com->setEnabled(settingsEnabled && b == 2);
    m_probe->setEnabled(settingsEnabled);
    if (m_scpiSimulate) {
        m_scpiSimulate->setVisible(b == 0);
    }
    m_connectMs->setEnabled(settingsEnabled);
    m_sweepMs->setEnabled(settingsEnabled);
    m_retries->setEnabled(settingsEnabled);
    updateDutEndpointEnabled();
    updateScpiQueueVisibility();
}

void ConnectionBar::emitTimeoutsChanged()
{
    if (m_timeoutsLocked) {
        return;
    }
    saveSettings();
    emit vnaSettingsChanged();
}

void ConnectionBar::loadSettings()
{
    QSettings s;
    const int backend = s.value(QStringLiteral("vna/backend"), 1).toInt();
    m_backend->setCurrentIndex(qBound(0, backend, 3));
    m_host->setText(s.value(QStringLiteral("vna/host"), QStringLiteral("127.0.0.1")).toString());
    m_port->setValue(s.value(QStringLiteral("vna/port"), 5025).toInt());
    m_com->setText(s.value(QStringLiteral("vna/com"), QStringLiteral("COM3")).toString());
    m_connectMs->blockSignals(true);
    m_sweepMs->blockSignals(true);
    m_retries->blockSignals(true);
    m_connectMs->setValue(s.value(QStringLiteral("vna/connect_ms"), 3000).toInt());
    m_sweepMs->setValue(s.value(QStringLiteral("vna/sweep_ms"), 30000).toInt());
    m_retries->setValue(s.value(QStringLiteral("vna/retries"), 2).toInt());
    m_connectMs->blockSignals(false);
    m_sweepMs->blockSignals(false);
    m_retries->blockSignals(false);

    const int ctrl = 0;  // реальный контроллер отсутствует; старые настройки не активируем
    m_ctrlBackend->blockSignals(true);
    m_ctrlBackend->setCurrentIndex(m_ctrlBackend->findData(ctrl));
    m_ctrlBackend->blockSignals(false);
    m_lastCtrlBackendIndex = m_ctrlBackend->currentIndex();
    m_dutHost->setText(
        s.value(QStringLiteral("dut/host"), QStringLiteral("192.168.0.10")).toString());
    m_dutPort->setValue(s.value(QStringLiteral("dut/port"), 4001).toInt());
    m_dutCom->setText(s.value(QStringLiteral("dut/com"), QStringLiteral("COM4")).toString());
    disableCombatControllerItem();
    updateFieldsEnabled();
}

void ConnectionBar::saveSettings() const
{
    QSettings s;
    s.setValue(QStringLiteral("vna/backend"), vnaBackend());
    s.setValue(QStringLiteral("vna/host"), vnaHost());
    s.setValue(QStringLiteral("vna/port"), vnaPort());
    s.setValue(QStringLiteral("vna/com"), vnaComPort());
    s.setValue(QStringLiteral("vna/connect_ms"), vnaConnectTimeoutMs());
    s.setValue(QStringLiteral("vna/sweep_ms"), vnaSweepTimeoutMs());
    s.setValue(QStringLiteral("vna/retries"), vnaMeasureRetries());
    s.setValue(QStringLiteral("dut/backend"), controllerBackend());
    s.setValue(QStringLiteral("dut/host"), dutHost());
    s.setValue(QStringLiteral("dut/port"), dutPort());
    s.setValue(QStringLiteral("dut/com"), dutComPort());
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

int ConnectionBar::vnaConnectTimeoutMs() const
{
    return m_connectMs->value();
}

int ConnectionBar::vnaSweepTimeoutMs() const
{
    return m_sweepMs->value();
}

int ConnectionBar::vnaMeasureRetries() const
{
    return m_retries->value();
}

int ConnectionBar::controllerBackend() const
{
    return m_ctrlBackend->currentData().toInt();
}

QString ConnectionBar::dutHost() const
{
    return m_dutHost->text().trimmed();
}

int ConnectionBar::dutPort() const
{
    return m_dutPort->value();
}

QString ConnectionBar::dutComPort() const
{
    return m_dutCom->text().trimmed();
}

void ConnectionBar::setVnaSettingsLocked(bool locked)
{
    m_timeoutsLocked = locked;
    updateFieldsEnabled();
}

void ConnectionBar::appendScpiErrors(const QStringList& entries)
{
    if (entries.isEmpty()) {
        return;
    }
    for (const QString& e : entries) {
        const QString line = formatScpiErrorLine(e);
        if (!line.isEmpty()) {
            m_scpiErrors->addItem(line);
        }
    }
    m_scpiErrors->scrollToBottom();
    updateScpiQueueVisibility();
}

void ConnectionBar::clearScpiErrorQueue()
{
    m_scpiErrors->clear();
    updateScpiQueueVisibility();
}

void ConnectionBar::updateScpiQueueVisibility()
{
    m_scpiPanel->setVisible(m_scpiErrors->count() > 0 || m_scpiSimulate->isVisible());
}

void ConnectionBar::setVnaInfo(const QString& model,
                               const QString& address,
                               bool connected,
                               const QString& serial,
                               const QString& firmware)
{
    QString detail = model;
    if (!serial.isEmpty()) {
        detail += QStringLiteral(" · SN %1").arg(serial);
    }
    if (!firmware.isEmpty()) {
        detail += QStringLiteral(" · v%1").arg(firmware);
    }
    if (connected) {
        m_vna->setText(QStringLiteral("C2220: %1 @ %2").arg(detail, address));
        m_vna->setStyleSheet(QStringLiteral("color:#0a7a2f;"));
        m_filterReady->setText(QStringLiteral("S-параметры: ГОТОВО"));
        m_filterReady->setStyleSheet(QStringLiteral("color:#0a7a2f; font-weight:700;"));
    } else {
        m_vna->setText(QStringLiteral("C2220: нет связи (%1 @ %2)").arg(detail, address));
        m_vna->setStyleSheet(QStringLiteral("color:#8a1f11;"));
        m_filterReady->setText(QStringLiteral("S-параметры: нужна связь VNA"));
        m_filterReady->setStyleSheet(QStringLiteral("color:#8a1f11; font-weight:700;"));
    }
}

void ConnectionBar::setControllerInfo(const QString& iface, bool connected)
{
    const bool stub = iface.contains(QStringLiteral("stub"), Qt::CaseInsensitive);
    const bool simulator = iface.contains(QStringLiteral("DutSimulator"), Qt::CaseInsensitive)
        || iface.contains(QStringLiteral("sim"), Qt::CaseInsensitive)
        || iface.contains(QStringLiteral("имитатор"), Qt::CaseInsensitive);
    if (stub) {
        m_controller->setText(
            QStringLiteral("CTRL: stub — протокол т.14 не передан (%1)").arg(iface));
        m_controller->setStyleSheet(QStringLiteral("color:#9a6700; font-weight:600;"));
        return;
    }
    if (simulator) {
        m_controller->setText(
            QStringLiteral("CTRL: ДЕМО — DutSimulator (для пассивных S-параметров не нужен)"));
        m_controller->setStyleSheet(QStringLiteral("color:#9a6700; font-weight:600;"));
        return;
    }
    if (iface.contains(QStringLiteral("отсутств"), Qt::CaseInsensitive)) {
        m_controller->setText(QStringLiteral(
            "CTRL: НЕТ КОНТРОЛЛЕРА — реальная калибровка 64×64 недоступна"));
        m_controller->setStyleSheet(QStringLiteral("color:#8a1f11; font-weight:600;"));
        return;
    }
    if (connected) {
        m_controller->setText(QStringLiteral("CTRL: OK — %1").arg(iface));
        m_controller->setStyleSheet(QStringLiteral("color:#0a7a2f; font-weight:600;"));
    } else {
        m_controller->setText(QStringLiteral("CTRL: нет связи (%1)").arg(iface));
        m_controller->setStyleSheet(QStringLiteral("color:#8a1f11; font-weight:600;"));
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
