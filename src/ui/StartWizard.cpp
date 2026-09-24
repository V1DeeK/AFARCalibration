#include "StartWizard.h"

#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QStringConverter>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWizardPage>

namespace {

QWizardPage* makeCheckPage(const QString& title, const QString& text, QCheckBox** outBox)
{
    auto* page = new QWizardPage();
    page->setTitle(title);
    auto* layout = new QVBoxLayout(page);
    auto* lab = new QLabel(text, page);
    lab->setWordWrap(true);
    layout->addWidget(lab);
    auto* box = new QCheckBox(QStringLiteral("Подтверждаю"), page);
    layout->addWidget(box);
    layout->addStretch(1);
    *outBox = box;
    return page;
}

}  // namespace

StartWizard::StartWizard(QWidget* parent)
    : QWizard(parent)
{
    setWindowTitle(QStringLiteral("Мастер запуска серии"));
    setMinimumWidth(560);
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::HaveHelpButton, true);
    setOption(QWizard::NoBackButtonOnStartPage, true);
    setButtonText(QWizard::NextButton, QStringLiteral("Далее"));
    setButtonText(QWizard::BackButton, QStringLiteral("Назад"));
    setButtonText(QWizard::CancelButton, QStringLiteral("Отмена"));
    setButtonText(QWizard::FinishButton, QStringLiteral("Готово — подготовить серию"));
    setButtonText(QWizard::HelpButton, QStringLiteral("Как пользоваться"));
    connect(this, &QWizard::helpRequested, this, &StartWizard::onHelpRequested);
    buildPages();
}

void StartWizard::onHelpRequested()
{
    QMessageBox::information(
        this, QStringLiteral("Как пользоваться мастером"),
        QStringLiteral(
            "Это обязательный обход т. 4.2 ТЗ перед запуском полной серии АФАР.\n\n"
            "• На шаге с галочкой отметьте «Подтверждаю», иначе «Далее» не пустит "
            "и покажет причину.\n"
            "• THRU должен быть ≤ 0,20 дБ и ≤ 2,0° (пороги можно править, это не метрология).\n"
            "• «Отмена» ничего не шлёт в прибор (на имитаторе SCPI нет).\n"
            "• «Готово» создаёт каталог серии и доводит автомат до READY.\n"
            "• Затем закройте мастер и нажмите зелёную «Старт серии АФАР»."));
}

void StartWizard::setSweepPreset(double fStartGhz,
                                 double fStopGhz,
                                 int points,
                                 int ifbwHz,
                                 double powerDbm,
                                 int averages)
{
    m_fStartGhz = fStartGhz;
    m_fStopGhz = fStopGhz;
    m_points = points;
    m_ifbwHz = ifbwHz;
    m_averages = averages;
    if (m_power) {
        m_power->setValue(powerDbm);
    }
}

void StartWizard::setVnaEndpoint(const QString& host, int port)
{
    m_vnaHost = host.trimmed().isEmpty() ? QStringLiteral("127.0.0.1") : host.trimmed();
    m_vnaPort = (port > 0 && port < 65536) ? port : 5025;
}

void StartWizard::buildPages()
{
    {
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("1. Подключения и питание"));
        auto* layout = new QVBoxLayout(page);
        auto* hint = new QLabel(
            QStringLiteral("Проверьте кабели C1220/C2220, контроллер и питание. "
                           "Сейчас работают программные имитаторы — галочка обязательна, "
                           "иначе шаг не закроется."),
            page);
        hint->setWordWrap(true);
        layout->addWidget(hint);
        m_dataRoot = new QLineEdit(page);
        const auto base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        m_dataRoot->setText(QDir(base).filePath(QStringLiteral("data")));
        layout->addWidget(new QLabel(QStringLiteral("Каталог данных серий:"), page));
        layout->addWidget(m_dataRoot);
        m_powerOk = new QCheckBox(QStringLiteral("Питание и соединения в норме"), page);
        m_engineer = new QCheckBox(
            QStringLiteral("Инженерный профиль (опасные режимы, UI-06)"), page);
        {
            QSettings settings;
            m_engineer->setChecked(
                settings.value(QStringLiteral("ui/engineer_profile"), false).toBool());
        }
        layout->addWidget(m_powerOk);
        layout->addWidget(m_engineer);
        connect(m_engineer, &QCheckBox::toggled, this, [](bool on) {
            QSettings settings;
            settings.setValue(QStringLiteral("ui/engineer_profile"), on);
        });
        addPage(page);
    }

    addPage(makeCheckPage(
        QStringLiteral("2. Идентификация VNA и контроллера"),
        QStringLiteral("На имитаторах *IDN? содержит C2220; живой VNA может быть C1220/C2220. "
                       "Отметьте «Подтверждаю», чтобы идти дальше."),
        &m_idnOk));

    addPage(makeCheckPage(
        QStringLiteral("3. Калибровка ВАЦ (флаг профиля)"),
        QStringLiteral("Это проверка идентификатора/флага калибровки из профиля, "
                       "не подмена метрологической аттестации."),
        &m_calOk));

    {
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("4. Проверка THRU"));
        auto* layout = new QVBoxLayout(page);
        auto* lab = new QLabel(
            QStringLiteral("Пороги по умолчанию FR-05: ≤0,20 дБ / ≤2,0°. "
                           "Не выдавать за утверждённые метрологом. "
                           "На имитаторе впишите контрольные значения в пределах порога."),
            page);
        lab->setWordWrap(true);
        layout->addWidget(lab);
        m_thruMag = new QDoubleSpinBox(page);
        m_thruMag->setRange(0.0, 5.0);
        m_thruMag->setDecimals(2);
        m_thruMag->setValue(0.05);
        m_thruMag->setSuffix(QStringLiteral(" дБ"));
        m_thruPhase = new QDoubleSpinBox(page);
        m_thruPhase->setRange(0.0, 20.0);
        m_thruPhase->setDecimals(2);
        m_thruPhase->setValue(0.5);
        m_thruPhase->setSuffix(QStringLiteral(" °"));
        layout->addWidget(new QLabel(QStringLiteral("Измеренный |Δ|:"), page));
        layout->addWidget(m_thruMag);
        layout->addWidget(new QLabel(QStringLiteral("Измеренный ∠Δ:"), page));
        layout->addWidget(m_thruPhase);
        addPage(page);
    }

    addPage(makeCheckPage(
        QStringLiteral("5. Отсутствие перегрузки"),
        QStringLiteral("Убедитесь, что приёмник не в перегрузке, и отметьте «Подтверждаю»."),
        &m_noOverload));

    addPage(makeCheckPage(
        QStringLiteral("6. Пробные коды"),
        QStringLiteral("Крайние и средние коды фазы/att прогонит оркестратор "
                       "(на имитаторе — компактный probe-профиль). Отметьте «Подтверждаю»."),
        &m_probeOk));

    {
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("7. Мощность и опасные режимы"));
        auto* layout = new QVBoxLayout(page);
        auto* lab = new QLabel(
            QStringLiteral("Мощность берётся из вкладки «Перебор кодов». "
                           "Direct access и принудительный safe_state — только инженерный профиль."),
            page);
        lab->setWordWrap(true);
        layout->addWidget(lab);
        m_power = new QDoubleSpinBox(page);
        m_power->setRange(-60.0, 10.0);
        m_power->setDecimals(1);
        m_power->setValue(-30.0);
        m_power->setSuffix(QStringLiteral(" дБм"));
        layout->addWidget(m_power);
        m_directAccess = new QCheckBox(
            QStringLiteral("Запросить direct access (опасный режим)"), page);
        m_forceSafe = new QCheckBox(
            QStringLiteral("Принудительный safe_state перед серией"), page);
        layout->addWidget(m_directAccess);
        layout->addWidget(m_forceSafe);
        addPage(page);
    }

    {
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("8. Температура и старт серии"));
        auto* layout = new QVBoxLayout(page);
        m_tempHint = new QLabel(
            QStringLiteral("После «Готово» оркестратор выполнит prepare → READY "
                           "на VnaSimulator + DutSimulator. Температура имитатора "
                           "появится в верхней полосе. Затем нажмите зелёную "
                           "«Старт серии АФАР»."),
            page);
        m_tempHint->setWordWrap(true);
        layout->addWidget(m_tempHint);
        addPage(page);
    }
}

void StartWizard::initializePage(int id)
{
    QWizard::initializePage(id);
    if (id == 6) {
        applyEngineerGate();
    }
}

void StartWizard::applyEngineerGate()
{
    const bool eng = engineerProfile();
    m_directAccess->setEnabled(eng);
    m_forceSafe->setEnabled(eng);
    if (!eng) {
        m_directAccess->setChecked(false);
        m_forceSafe->setChecked(false);
        if (m_power->value() > -20.0) {
            m_power->setValue(-20.0);
        }
    }
}

QString StartWizard::dataRoot() const
{
    return m_dataRoot->text().trimmed();
}

QString StartWizard::runConfigPath() const
{
    return m_cfgPath;
}

QString StartWizard::attenuatorCsvPath() const
{
    return m_csvPath;
}

double StartWizard::powerDbm() const
{
    return m_power->value();
}

bool StartWizard::directAccessRequested() const
{
    return m_directAccess->isChecked();
}

bool StartWizard::forceSafeState() const
{
    return m_forceSafe->isChecked();
}

bool StartWizard::engineerProfile() const
{
    return m_engineer && m_engineer->isChecked();
}

bool StartWizard::connectionsConfirmed() const
{
    return m_powerOk && m_powerOk->isChecked();
}

bool StartWizard::idnConfirmed() const
{
    return m_idnOk && m_idnOk->isChecked();
}

bool StartWizard::calConfirmed() const
{
    return m_calOk && m_calOk->isChecked();
}

bool StartWizard::noOverloadConfirmed() const
{
    return m_noOverload && m_noOverload->isChecked();
}

bool StartWizard::probeConfirmed() const
{
    return m_probeOk && m_probeOk->isChecked();
}

QString StartWizard::thruSummary() const
{
    if (!m_thruMag || !m_thruPhase) {
        return QStringLiteral("не измерено");
    }
    return QStringLiteral("%1 дБ / %2°")
        .arg(m_thruMag->value(), 0, 'f', 2)
        .arg(m_thruPhase->value(), 0, 'f', 1);
}

bool StartWizard::confirmDangerousSettings()
{
    applyEngineerGate();
    const bool dangerousPower = m_power->value() > -20.0;
    const bool dangerous = dangerousPower || m_directAccess->isChecked()
        || m_forceSafe->isChecked();
    if (!dangerous) {
        return true;
    }
    if (!engineerProfile()) {
        QMessageBox::warning(
            this, QStringLiteral("Инженерный профиль"),
            QStringLiteral("Опасные настройки доступны только при включённом "
                           "инженерном профиле на шаге 1 (UI-06)."));
        return false;
    }
    QStringList reasons;
    if (dangerousPower) {
        reasons << QStringLiteral("мощность %1 дБм (> −20 дБм)").arg(m_power->value());
    }
    if (m_directAccess->isChecked()) {
        reasons << QStringLiteral("direct access");
    }
    if (m_forceSafe->isChecked()) {
        reasons << QStringLiteral("принудительный safe_state");
    }
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Подтверждение инженера"),
        QStringLiteral("Опасные настройки: %1.\n\nПовторите подтверждение для продолжения.")
            .arg(reasons.join(QStringLiteral(", "))),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return answer == QMessageBox::Yes;
}

bool StartWizard::validateCurrentPage()
{
    const int id = currentId();
    if (id == 0 && !m_powerOk->isChecked()) {
        QMessageBox::information(this, QStringLiteral("Шаг не завершён"),
                                 QStringLiteral("Отметьте «Питание и соединения в норме»."));
        return false;
    }
    if (id == 1 && !m_idnOk->isChecked()) {
        QMessageBox::information(this, QStringLiteral("Шаг не завершён"),
                                 QStringLiteral("Подтвердите идентификацию VNA и контроллера."));
        return false;
    }
    if (id == 2 && !m_calOk->isChecked()) {
        QMessageBox::information(this, QStringLiteral("Шаг не завершён"),
                                 QStringLiteral("Подтвердите флаг калибровки ВАЦ."));
        return false;
    }
    if (id == 3) {
        if (m_thruMag->value() > 0.20 || m_thruPhase->value() > 2.0) {
            QMessageBox::warning(
                this, QStringLiteral("THRU вне порога"),
                QStringLiteral("Значения превышают порог по умолчанию 0,20 дБ / 2,0°. "
                               "Скорректируйте или прервите запуск."));
            return false;
        }
    }
    if (id == 4 && !m_noOverload->isChecked()) {
        QMessageBox::information(this, QStringLiteral("Шаг не завершён"),
                                 QStringLiteral("Подтвердите отсутствие перегрузки."));
        return false;
    }
    if (id == 5 && !m_probeOk->isChecked()) {
        QMessageBox::information(this, QStringLiteral("Шаг не завершён"),
                                 QStringLiteral("Подтвердите пробные коды."));
        return false;
    }
    if (id == 6) {
        return confirmDangerousSettings();
    }
    return true;
}

bool StartWizard::materializeSimFixtures(QString& diagnostics)
{
    diagnostics.clear();
    QDir root(dataRoot());
    if (!root.mkpath(QStringLiteral("."))) {
        diagnostics = QStringLiteral("Не удалось создать каталог данных");
        return false;
    }
    QDir fixtures(root.filePath(QStringLiteral("_sim_fixtures")));
    if (!fixtures.mkpath(QStringLiteral("."))) {
        diagnostics = QStringLiteral("Не удалось создать каталог фикстур");
        return false;
    }

    m_cfgPath = fixtures.filePath(QStringLiteral("run-config.json"));
    m_csvPath = fixtures.filePath(QStringLiteral("attenuator-codes.csv"));

    const double power = m_power->value();
    const qint64 fStart = static_cast<qint64>(m_fStartGhz * 1e9 + 0.5);
    const qint64 fStop = static_cast<qint64>(m_fStopGhz * 1e9 + 0.5);
    const QString runId =
        QStringLiteral("SIM-UI-%1").arg(QDateTime::currentDateTimeUtc().toString(
            QStringLiteral("yyyyMMdd-hhmmss")));
    QFile cfg(m_cfgPath);
    if (!cfg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        diagnostics = QStringLiteral("Не удалось записать run-config.json");
        return false;
    }
    QTextStream out(&cfg);
    out.setEncoding(QStringConverter::Utf8);
    out << "{\n"
           "  \"schema\": \"afar.stage1.run-config/v1\",\n"
           "  \"run_id\": \""
        << runId
        << "\",\n"
           "  \"vna\": {\n"
           "    \"model\": \"PLANAR C2220\",\n"
           "    \"host\": \""
        << m_vnaHost
        << "\",\n"
           "    \"port\": "
        << m_vnaPort
        << ",\n"
           "    \"s_parameter\": \"S21\",\n"
           "    \"f_start_hz\": "
        << fStart
        << ",\n"
           "    \"f_stop_hz\": "
        << fStop
        << ",\n"
           "    \"points\": "
        << m_points
        << ",\n"
           "    \"ifbw_hz\": "
        << m_ifbwHz
        << ",\n"
           "    \"power_dbm\": "
        << power
        << ",\n"
           "    \"averages\": "
        << m_averages
        << "\n"
           "  },\n"
           "  \"controller\": { \"driver\": \"sim\", \"endpoint\": \"sim\" },\n"
           "  \"dut\": {\n"
           "    \"serial\": \"SIM-UI\",\n"
           "    \"channels\": { \"first\": 1, \"last\": 1 },\n"
           "    \"phase_codes\": { \"first\": 0, \"last\": 3, \"lsb_deg\": 5.625 },\n"
           "    \"attenuator_codes_file\": \"attenuator-codes.csv\",\n"
           "    \"reference\": { \"att_code\": 0, \"phase_code\": 0 }\n"
           "  },\n"
           "  \"timing\": { \"settle_ms\": 0, \"reference_after_phase_row\": true },\n"
           "  \"limits\": { \"max_drift_phase_deg\": 1.0, \"max_phase_residual_deg\": 2.8125 }\n"
           "}\n";
    cfg.close();

    QFile csv(m_csvPath);
    if (!csv.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        diagnostics = QStringLiteral("Не удалось записать attenuator-codes.csv");
        return false;
    }
    QTextStream csvOut(&csv);
    csvOut.setEncoding(QStringConverter::Utf8);
    csvOut << "att_code,att_cmd_db,enabled,settle_ms\n"
              "0,0.00,true,0\n"
              "1,0.50,true,0\n"
              "2,1.00,false,0\n";
    csv.close();
    return true;
}
