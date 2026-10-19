#include "ConnectionBar.h"

#include "UiStyle.h"

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
#include <QSizePolicy>
#include <QSpinBox>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTabWidget>
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

class SharedPanelTabs final : public QTabWidget {
public:
    explicit SharedPanelTabs(QWidget* parent)
        : QTabWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        connect(tabBar(), &QTabBar::tabBarClicked, this, [this](int index) {
            if (m_expanded && index == m_openIndex) {
                m_expanded = false;
                setMaximumHeight(tabBar()->sizeHint().height() + 2);
            } else {
                m_openIndex = index;
                setCurrentIndex(index);
                m_expanded = true;
                setMaximumHeight(QWIDGETSIZE_MAX);
            }
            updateGeometry();
        });
        connect(this, &QTabWidget::currentChanged, this, [this]() { updateGeometry(); });
    }

    QSize sizeHint() const override
    {
        QSize preferred = QTabWidget::sizeHint();
        const int tabsHeight = tabBar()->sizeHint().height();
        if (!m_expanded) {
            preferred.setHeight(tabsHeight + 2);
        } else if (currentWidget()) {
            preferred.setHeight(tabsHeight + currentWidget()->sizeHint().height()
                                + 2 * ui::kInnerGap);
        }
        return preferred;
    }

private:
    int m_openIndex{0};
    bool m_expanded{true};
};

}  // namespace

void ConnectionBar::setMiddleWidget(QWidget* widget)
{
    m_panels->addTab(widget, QStringLiteral("Калибровка ВАЦ"));
}

ConnectionBar::ConnectionBar(QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    setObjectName(QStringLiteral("ConnectionBar"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(ui::kGap);

    // Состояния — «таблетки» одного вида; цвет задаёт тон, а не жёсткий цвет текста.
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
    for (auto* label : {m_source, m_filterReady, m_vna, m_controller, m_temperature, m_status}) {
        label->setWordWrap(true);
        label->setProperty("role", QStringLiteral("status"));
    }
    ui::setTone(m_source, ui::Tone::Warn);
    ui::setTone(m_filterReady, ui::Tone::Error);
    ui::setTone(m_vna, ui::Tone::Error);
    ui::setTone(m_controller, ui::Tone::Warn);

    // Тему переключает кнопка справа от вкладок; своя кнопка полосы осталась только для совместимости.
    m_theme = new QPushButton(QStringLiteral("Тема: светлая"), this);
    m_theme->setObjectName(QStringLiteral("btnTheme"));
    m_theme->setVisible(false);
    connect(m_theme, &QPushButton::clicked, this, &ConnectionBar::themeToggleRequested);

    // Поля подключения — как в окошке «Подключение к плате»: подпись над полем, статус, кнопка.
    const auto caption = [this](const QString& text) { return ui::captionLabel(text, this); };

    // --- Подключение к прибору ---
    auto* vnaBox = new QWidget(this);
    vnaBox->setObjectName(QStringLiteral("vnaConnectionBox"));
    auto* vnaLayout = new QVBoxLayout(vnaBox);
    vnaLayout->setSpacing(ui::kInnerGap);
    m_backend = new QComboBox(this);
    m_backend->setObjectName(QStringLiteral("vnaBackend"));
    m_backend->addItem(QStringLiteral("Демостенд"), 0);
    m_backend->setItemData(0, QStringLiteral("Имитатор VNA + DUT без оборудования"), Qt::ToolTipRole);
    m_backend->addItem(QStringLiteral("C2220 через S2VNA"), 1);
    m_backend->setItemData(
        1,
        QStringLiteral(
            "Живой C1220/C2220 по TCP. При проверке связи S2VNA запускается скрыто, "
            "если она ещё не запущена."),
        Qt::ToolTipRole);
    m_backend->setToolTip(
        QStringLiteral("Реальный C2220 подключается через S2VNA; Демостенд работает локально."));
    m_host = new QLineEdit(QStringLiteral("127.0.0.1"), this);
    m_host->setObjectName(QStringLiteral("vnaHost"));
    m_port = new QSpinBox(this);
    m_port->setObjectName(QStringLiteral("vnaPort"));
    m_port->setRange(1, 65535);
    m_port->setValue(5025);
    m_port->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_probe = new QPushButton(QStringLiteral("Вкл"), this);
    m_probe->setObjectName(QStringLiteral("btnPrimary"));
    ui::setButtonRole(m_probe, ui::ButtonRole::Primary);
    auto* vnaFields = new QGridLayout;
    vnaFields->setHorizontalSpacing(ui::kInnerGap);
    vnaFields->setVerticalSpacing(2);
    vnaFields->addWidget(caption(QStringLiteral("Адрес (Host)")), 0, 0);
    vnaFields->addWidget(caption(QStringLiteral("Порт")), 0, 1);
    vnaFields->addWidget(m_host, 1, 0);
    vnaFields->addWidget(m_port, 1, 1);
    vnaFields->setColumnStretch(0, 1);
    vnaLayout->addWidget(caption(QStringLiteral("Тип подключения")));
    vnaLayout->addWidget(m_backend);
    vnaLayout->addLayout(vnaFields);
    vnaLayout->addWidget(m_vna);
    vnaLayout->addWidget(m_filterReady);
    vnaLayout->addWidget(m_probe);
    vnaLayout->addStretch(1);

    // --- Контроллер изделия и состояние: вторая страница общей области ---
    auto* stateBox = new QWidget(this);
    stateBox->setObjectName(QStringLiteral("stateBox"));
    auto* stateLayout = new QVBoxLayout(stateBox);
    stateLayout->setSpacing(ui::kInnerGap);
    m_ctrlBackend = new QComboBox(this);
    m_ctrlBackend->setObjectName(QStringLiteral("controllerBackend"));
    m_ctrlBackend->addItem(QStringLiteral("Демо-контроллер (DutSimulator)"), 0);
    m_ctrlBackend->addItem(QStringLiteral("PS изделия (JSON, TCP :49600)"), 3);
    m_dutHost = new QLineEdit(QStringLiteral("192.168.0.10"), this);
    m_dutHost->setToolTip(QStringLiteral("адрес PS из профиля стенда"));
    m_dutPort = new QSpinBox(this);
    m_dutPort->setRange(1, 65535);
    m_dutPort->setValue(4001);
    m_dutPort->setButtonSymbols(QAbstractSpinBox::NoButtons);
    m_dutPort->setToolTip(QStringLiteral("TCP-порт канала управления PS"));
    m_dutCom = new QLineEdit(QStringLiteral("COM4"), this);
    m_dutCom->setToolTip(QStringLiteral("нужен протокол т.14"));
    auto* dutFields = new QGridLayout;
    dutFields->setHorizontalSpacing(ui::kInnerGap);
    dutFields->setVerticalSpacing(2);
    dutFields->addWidget(caption(QStringLiteral("Адрес (Host)")), 0, 0);
    dutFields->addWidget(caption(QStringLiteral("Порт")), 0, 1);
    dutFields->addWidget(m_dutHost, 1, 0);
    dutFields->addWidget(m_dutPort, 1, 1);
    dutFields->setColumnStretch(0, 1);
    stateLayout->addWidget(caption(QStringLiteral("Контроллер")));
    stateLayout->addWidget(m_ctrlBackend);
    stateLayout->addLayout(dutFields);
    stateLayout->addWidget(caption(QStringLiteral("COM-порт")));
    stateLayout->addWidget(m_dutCom);
    stateLayout->addWidget(m_controller);
    stateLayout->addWidget(m_source);
    stateLayout->addWidget(m_temperature);
    stateLayout->addWidget(m_status);
    stateLayout->addStretch(1);

    // Разделы используют одно общее пространство и меняются только выбором вкладки.
    m_panels = new SharedPanelTabs(this);
    m_panels->setObjectName(QStringLiteral("connectionTabs"));
    m_panels->setUsesScrollButtons(false);
    m_panels->addTab(vnaBox, QStringLiteral("Подключение"));
    m_panels->addTab(stateBox, QStringLiteral("Контроллер"));
    root->addWidget(m_panels);
    disableCombatControllerItem();

    m_diagnostic = new QLabel(vnaBox);
    m_diagnostic->setWordWrap(true);
    m_diagnostic->setVisible(false);
    m_diagnostic->setProperty("role", QStringLiteral("status"));
    ui::setTone(m_diagnostic, ui::Tone::Warn);
    vnaLayout->insertWidget(vnaLayout->count() - 1, m_diagnostic);

    // UI-ERR-001: немодальная очередь SCPI (не QMessageBox).
    m_scpiPanel = new QWidget(this);
    auto* scpiLay = new QVBoxLayout(m_scpiPanel);
    scpiLay->setContentsMargins(0, 2, 0, 0);
    scpiLay->setSpacing(2);
    auto* scpiHead = new QHBoxLayout();
    auto* scpiTitle = new QLabel(QStringLiteral("Очередь ошибок SCPI"), m_scpiPanel);
    scpiTitle->setStyleSheet(QStringLiteral("font-weight:600;"));
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
    scpiLay->addWidget(m_scpiErrors);
    m_scpiPanel->setVisible(false);
    root->addWidget(m_scpiPanel);

    // UI-TO-001: тайм-ауты VNA не показываются; значения берутся из QSettings (vna/*) и уходят в C2220Vna::Profile.
    m_connectMs = new QSpinBox(this);
    m_connectMs->setRange(100, 120000);
    m_connectMs->setValue(3000);
    m_sweepMs = new QSpinBox(this);
    m_sweepMs->setRange(1000, 600000);
    m_sweepMs->setValue(30000);
    m_retries = new QSpinBox(this);
    m_retries->setRange(0, 10);
    m_retries->setValue(2);
    for (QSpinBox* spin : {m_connectMs, m_sweepMs, m_retries}) {
        spin->hide();
    }

    connect(m_backend, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            &ConnectionBar::onBackendChanged);
    connect(m_probe, &QPushButton::clicked, this, [this]() {
        saveSettings();
        emit vnaConnectionToggleRequested(!m_vnaConnectionActive);
    });
    connect(m_host, &QLineEdit::editingFinished, this, [this]() {
        saveSettings();
        emit vnaSettingsChanged();
    });
    connect(m_port, &QSpinBox::editingFinished, this, [this]() {
        saveSettings();
        emit vnaSettingsChanged();
    });
    connect(m_scpiClear, &QPushButton::clicked, this, &ConnectionBar::clearScpiErrorQueue);
    connect(m_scpiSimulate, &QPushButton::clicked, this,
            &ConnectionBar::simulateScpiErrorRequested);
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
    if (mode == 3 && m_dutPort->value() == 4001) {
        m_dutPort->setValue(49600);
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
    // Адрес нужен только PS; COM у PS нет.
    const bool ps = controllerBackend() == 3;
    m_dutHost->setEnabled(ps);
    m_dutPort->setEnabled(ps);
    m_dutCom->setEnabled(false);
    m_dutCom->setToolTip(QStringLiteral("PS управляется только по TCP"));
}

void ConnectionBar::updateFieldsEnabled()
{
    const int b = vnaBackend();
    const bool settingsEnabled = !m_timeoutsLocked && !m_vnaConnectionActive;
    m_backend->setEnabled(settingsEnabled);
    m_host->setEnabled(settingsEnabled && b == 1);
    m_port->setEnabled(settingsEnabled && b == 1);
    m_probe->setEnabled(!m_timeoutsLocked);
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
    const int savedBackend = s.value(QStringLiteral("vna/backend"), 1).toInt();
    const int backend = savedBackend == 3 ? 0 : savedBackend == 2 ? 1 : savedBackend;
    const int backendIndex = m_backend->findData(backend);
    m_backend->setCurrentIndex(backendIndex >= 0 ? backendIndex : m_backend->findData(1));
    m_host->setText(s.value(QStringLiteral("vna/host"), QStringLiteral("127.0.0.1")).toString());
    m_port->setValue(s.value(QStringLiteral("vna/port"), 5025).toInt());
    m_connectMs->blockSignals(true);
    m_sweepMs->blockSignals(true);
    m_retries->blockSignals(true);
    m_connectMs->setValue(s.value(QStringLiteral("vna/connect_ms"), 3000).toInt());
    m_sweepMs->setValue(s.value(QStringLiteral("vna/sweep_ms"), 30000).toInt());
    m_retries->setValue(s.value(QStringLiteral("vna/retries"), 2).toInt());
    m_connectMs->blockSignals(false);
    m_sweepMs->blockSignals(false);
    m_retries->blockSignals(false);

    // Сохранённый выбор PS восстанавливаем; прочее — деморежим.
    const int ctrl = s.value(QStringLiteral("dut/backend"), 0).toInt() == 3 ? 3 : 0;
    m_ctrlBackend->blockSignals(true);
    m_ctrlBackend->setCurrentIndex(m_ctrlBackend->findData(ctrl));
    m_ctrlBackend->blockSignals(false);
    m_lastCtrlBackendIndex = m_ctrlBackend->currentIndex();
    m_dutHost->setText(
        s.value(QStringLiteral("dut/host"), QStringLiteral("192.168.0.10")).toString());
    m_dutPort->setValue(s.value(QStringLiteral("dut/port"), 4001).toInt());
    m_dutCom->setText(s.value(QStringLiteral("dut/com"), QStringLiteral("COM4")).toString());
    disableCombatControllerItem();
    updateDutEndpointEnabled();
    updateFieldsEnabled();
}

void ConnectionBar::saveSettings() const
{
    QSettings s;
    s.setValue(QStringLiteral("vna/backend"), vnaBackend());
    s.setValue(QStringLiteral("vna/host"), vnaHost());
    s.setValue(QStringLiteral("vna/port"), vnaPort());
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

void ConnectionBar::setVnaConnectionActive(bool active)
{
    m_vnaConnectionActive = active;
    m_probe->setText(active ? QStringLiteral("Выкл") : QStringLiteral("Вкл"));
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
        ui::setTone(m_vna, ui::Tone::Ok);
        m_filterReady->setText(QStringLiteral("S-параметры: ГОТОВО"));
        ui::setTone(m_filterReady, ui::Tone::Ok);
    } else {
        m_vna->setText(QStringLiteral("C2220: нет связи (%1 @ %2)").arg(detail, address));
        ui::setTone(m_vna, ui::Tone::Error);
        m_filterReady->setText(QStringLiteral("S-параметры: нужна связь VNA"));
        ui::setTone(m_filterReady, ui::Tone::Error);
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
        ui::setTone(m_controller, ui::Tone::Warn);
        return;
    }
    if (simulator) {
        m_controller->setText(
            QStringLiteral("CTRL: ДЕМО — DutSimulator (для пассивных S-параметров не нужен)"));
        ui::setTone(m_controller, ui::Tone::Warn);
        return;
    }
    if (iface.contains(QStringLiteral("отсутств"), Qt::CaseInsensitive)) {
        m_controller->setText(QStringLiteral(
            "CTRL: НЕТ КОНТРОЛЛЕРА — реальная калибровка 64×64 недоступна"));
        ui::setTone(m_controller, ui::Tone::Error);
        return;
    }
    if (connected) {
        m_controller->setText(QStringLiteral("CTRL: OK — %1").arg(iface));
        ui::setTone(m_controller, ui::Tone::Ok);
    } else {
        m_controller->setText(QStringLiteral("CTRL: нет связи (%1)").arg(iface));
        ui::setTone(m_controller, ui::Tone::Error);
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
    // Цвет состояния приходит из рабочего потока строкой; на экране это тон «таблетки».
    ui::Tone tone = ui::Tone::Neutral;
    if (colorName == QLatin1String("#0a7a2f")) {
        tone = ui::Tone::Ok;
    } else if (colorName == QLatin1String("#0b5cab")) {
        tone = ui::Tone::Info;
    } else if (colorName == QLatin1String("#9a6700")) {
        tone = ui::Tone::Warn;
    } else if (colorName == QLatin1String("#8a1f11")) {
        tone = ui::Tone::Error;
    }
    ui::setTone(m_status, tone);
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
    const bool live = text.contains(QStringLiteral("живой"), Qt::CaseInsensitive);
    ui::setTone(m_source, live ? ui::Tone::Ok : ui::Tone::Warn);
}
