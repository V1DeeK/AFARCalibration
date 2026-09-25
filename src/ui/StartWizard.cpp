#include "StartWizard.h"

#include <QCheckBox>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QSettings>
#include <QSignalBlocker>
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
            "Это обязательный обход т. 4.2 ТЗ перед Старт.\n\n"
            "• Калибровка ВАЦ — SOLT из AFAR (этап 2) или чеклист Response/Thru в мастере; "
            "SCPI через текущий VNA-транспорт.\n"
            "• THRU: пороги mag_db / phase_deg редактируемы (дефолт 0,20 дБ / 2,0°) — "
            "настройки ПО, не метрология. Без утверждения метролога обычная серия не стартует.\n"
            "• Объём серии: компактный или полный AT-04 (все состояния профиля).\n"
            "• «Отмена» ничего не шлёт в прибор (на имитаторе SCPI нет).\n"
            "• «Готово» создаёт каталог серии и доводит автомат до READY.\n"
            "• Затем закройте мастер и нажмите зелёную «Старт»."));
}

void StartWizard::setSweepPreset(double fStartHz,
                                 double fStopHz,
                                 int points,
                                 int ifbwHz,
                                 double powerDbm,
                                 int averages)
{
    m_fStartHz = fStartHz;
    m_fStopHz = fStopHz;
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
            QStringLiteral("Проверьте кабели C2220, контроллер и питание. "
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

        layout->addWidget(new QLabel(QStringLiteral("Объём серии (AT-04):"), page));
        m_seriesCompact = new QRadioButton(
            QStringLiteral("Компактный (как сейчас: урезанная сетка)"), page);
        m_seriesFull = new QRadioButton(
            QStringLiteral("Полный AT-04 (каналы 1…16 × att 0…63 × фаза 0…63)"), page);
        m_seriesCompact->setChecked(true);
        {
            QSettings settings;
            if (settings.value(QStringLiteral("ui/series_volume_full"), false).toBool()) {
                m_seriesFull->setChecked(true);
            }
        }
        layout->addWidget(m_seriesCompact);
        layout->addWidget(m_seriesFull);
        auto persistVolume = [this](bool) {
            QSettings settings;
            settings.setValue(QStringLiteral("ui/series_volume_full"), fullSeriesVolume());
        };
        connect(m_seriesCompact, &QRadioButton::toggled, this, persistVolume);
        connect(m_seriesFull, &QRadioButton::toggled, this, persistVolume);
        addPage(page);
    }

    addPage(makeCheckPage(
        QStringLiteral("2. Идентификация C2220 и контроллера"),
        QStringLiteral("На имитаторах *IDN? содержит C2220; контроллер — DutSimulator. "
                       "Отметьте «Подтверждаю», чтобы идти дальше."),
        &m_idnOk));

    {
        // UI-205 / CAL-001 / CAL-002: калибровка только в S2VNA, чеклист без SCPI.
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("3. Калибровка ВАЦ (в S2VNA)"));
        auto* layout = new QVBoxLayout(page);
        auto* lab = new QLabel(
            QStringLiteral(
                "Полная калибровка ВАЦ (SOLT / Response / Thru) выполняется в программе "
                "S2VNA до серии; здесь только подтверждение оператора. "
                "AFAR не шлёт команды калибровки по SCPI.\n\n"
                "Порядок: docs/S2VNA-setup.md → калибровка в S2VNA → этот чеклист → серия."),
            page);
        lab->setWordWrap(true);
        layout->addWidget(lab);

        m_calStatus = new QLabel(page);
        m_calStatus->setWordWrap(true);
        layout->addWidget(m_calStatus);

        layout->addWidget(new QLabel(
            QStringLiteral("Чеклист Response / Thru (сделайте в S2VNA):"), page));
        m_calStepResponse = new QCheckBox(
            QStringLiteral("1. Выполнен Response (нормализация) в S2VNA"), page);
        m_calStepThru = new QCheckBox(
            QStringLiteral("2. Выполнен Thru в S2VNA (или эквивалент для тракта)"), page);
        m_calStepApplied = new QCheckBox(
            QStringLiteral("3. Калибровка применена к активному каналу S2VNA"), page);
        layout->addWidget(m_calStepResponse);
        layout->addWidget(m_calStepThru);
        layout->addWidget(m_calStepApplied);

        layout->addWidget(new QLabel(
            QStringLiteral("Идентификатор калибровки ВАЦ (вручную, опционально):"), page));
        m_vnaCalId = new QLineEdit(page);
        m_vnaCalId->setPlaceholderText(
            QStringLiteral("vna_calibration_id — из S2VNA / журнала, не SCPI"));
        {
            QSettings settings;
            m_vnaCalId->setText(
                settings.value(QStringLiteral("ui/vna_calibration_id")).toString());
        }
        layout->addWidget(m_vnaCalId);
        connect(m_vnaCalId, &QLineEdit::editingFinished, this,
                &StartWizard::persistVnaCalibrationId);

        m_calOk = new QCheckBox(
            QStringLiteral("Подтверждаю: калибровка выполнена в S2VNA"), page);
        layout->addWidget(m_calOk);
        layout->addStretch(1);

        const auto refresh = [this](bool) { refreshCalStatusLabel(); };
        connect(m_calStepResponse, &QCheckBox::toggled, this, refresh);
        connect(m_calStepThru, &QCheckBox::toggled, this, refresh);
        connect(m_calStepApplied, &QCheckBox::toggled, this, refresh);
        connect(m_calOk, &QCheckBox::toggled, this, refresh);
        refreshCalStatusLabel();
        addPage(page);
    }

    {
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("4. Проверка THRU (пороги FR-05)"));
        auto* layout = new QVBoxLayout(page);
        auto* lab = new QLabel(
            QStringLiteral(
                "После Thru введите измеренные отклонения и пороги сравнения. "
                "Дефолт FR-05: ≤0,20 дБ / ≤2,0° — настройки ПО, не утверждённая метрология. "
                "На имитаторе впишите контрольные значения в пределах порога."),
            page);
        lab->setWordWrap(true);
        layout->addWidget(lab);

        layout->addWidget(new QLabel(QStringLiteral("Порог |Δ| mag_db:"), page));
        m_thruMagLimit = new QDoubleSpinBox(page);
        m_thruMagLimit->setRange(0.01, 5.0);
        m_thruMagLimit->setDecimals(2);
        m_thruMagLimit->setValue(0.20);
        m_thruMagLimit->setSuffix(QStringLiteral(" дБ"));
        layout->addWidget(m_thruMagLimit);

        layout->addWidget(new QLabel(QStringLiteral("Порог ∠Δ phase_deg:"), page));
        m_thruPhaseLimit = new QDoubleSpinBox(page);
        m_thruPhaseLimit->setRange(0.01, 20.0);
        m_thruPhaseLimit->setDecimals(2);
        m_thruPhaseLimit->setValue(2.0);
        m_thruPhaseLimit->setSuffix(QStringLiteral(" °"));
        layout->addWidget(m_thruPhaseLimit);

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

        auto* metroTitle = new QLabel(QStringLiteral("Утверждение метрологом:"), page);
        metroTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
        layout->addWidget(metroTitle);
        m_metroApproved = new QCheckBox(
            QStringLiteral("Утверждено метрологом (не настройка ПО)"), page);
        m_metroName = new QLineEdit(page);
        m_metroName->setPlaceholderText(QStringLiteral("ФИО метролога"));
        m_metroDate = new QDateEdit(page);
        m_metroDate->setCalendarPopup(true);
        m_metroDate->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        m_metroDate->setDate(QDate::currentDate());
        layout->addWidget(m_metroApproved);
        layout->addWidget(new QLabel(QStringLiteral("ФИО:"), page));
        layout->addWidget(m_metroName);
        layout->addWidget(new QLabel(QStringLiteral("Дата:"), page));
        layout->addWidget(m_metroDate);
        {
            QSettings settings;
            m_thruMagLimit->setValue(
                settings.value(QStringLiteral("ui/thru_mag_limit_db"), 0.20).toDouble());
            m_thruPhaseLimit->setValue(
                settings.value(QStringLiteral("ui/thru_phase_limit_deg"), 2.0).toDouble());
            m_metroApproved->setChecked(
                settings.value(QStringLiteral("ui/metro_approved"), false).toBool());
            m_metroName->setText(settings.value(QStringLiteral("ui/metro_name")).toString());
            const auto d = QDate::fromString(
                settings.value(QStringLiteral("ui/metro_date")).toString(), Qt::ISODate);
            if (d.isValid()) {
                m_metroDate->setDate(d);
            }
        }
        connect(m_thruMagLimit, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { persistThruMeta(); });
        connect(m_thruPhaseLimit, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { persistThruMeta(); });
        connect(m_metroApproved, &QCheckBox::toggled, this, [this](bool) { persistThruMeta(); });
        connect(m_metroName, &QLineEdit::editingFinished, this, &StartWizard::persistThruMeta);
        connect(m_metroDate, &QDateEdit::dateChanged, this, [this](QDate) { persistThruMeta(); });
        addPage(page);
    }

    addPage(makeCheckPage(
        QStringLiteral("5. Отсутствие перегрузки"),
        QStringLiteral("Убедитесь, что приёмник не в перегрузке, и отметьте «Подтверждаю»."),
        &m_noOverload));

    {
        auto* page = new QWizardPage(this);
        page->setTitle(QStringLiteral("6. Пробные коды"));
        auto* layout = new QVBoxLayout(page);
        auto* lab = new QLabel(
            QStringLiteral(
                "По галочке оркестратор снимет короткий набор: канал 1, att 0 и 1, "
                "фазы 0 и 63 (на имитаторе и при выбранном живом VNA). "
                "Отмена мастера SCPI не шлёт. Дождитесь успеха до «Готово»."),
            page);
        lab->setWordWrap(true);
        layout->addWidget(lab);
        m_probeOk = new QCheckBox(QStringLiteral("Подтверждаю"), page);
        layout->addWidget(m_probeOk);
        m_probeStatus = new QLabel(QStringLiteral("Пробные коды не сняты."), page);
        m_probeStatus->setWordWrap(true);
        layout->addWidget(m_probeStatus);
        layout->addStretch(1);
        connect(m_probeOk, &QCheckBox::toggled, this, &StartWizard::onProbeCheckToggled);
        addPage(page);
    }

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
                           "появится в верхней полосе. Затем нажмите зелёную «Старт».\n\n"
                           "Без утверждения метролога обычная серия не стартует; "
                           "инженерный профиль — только с предупреждением."),
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
    return m_calOk && m_calOk->isChecked() && calChecklistComplete();
}

bool StartWizard::calChecklistComplete() const
{
    return m_calStepResponse && m_calStepResponse->isChecked()
        && m_calStepThru && m_calStepThru->isChecked()
        && m_calStepApplied && m_calStepApplied->isChecked();
}

QString StartWizard::vnaCalibrationId() const
{
    return m_vnaCalId ? m_vnaCalId->text().trimmed() : QString();
}

void StartWizard::refreshCalStatusLabel()
{
    if (!m_calStatus) {
        return;
    }
    const bool ok = calConfirmed();
    m_calStatus->setText(
        ok ? QStringLiteral("Калибровка ВАЦ: подтверждена")
           : QStringLiteral("Калибровка ВАЦ: нет"));
}

void StartWizard::persistVnaCalibrationId()
{
    if (!m_vnaCalId) {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("ui/vna_calibration_id"), m_vnaCalId->text().trimmed());
}

bool StartWizard::noOverloadConfirmed() const
{
    return m_noOverload && m_noOverload->isChecked();
}

bool StartWizard::probeConfirmed() const
{
    return m_probeOk && m_probeOk->isChecked() && m_probeCodesOk;
}

void StartWizard::onProbeCheckToggled(bool checked)
{
    if (!checked) {
        m_probeCodesOk = false;
        m_probeCodesPending = false;
        if (m_probeStatus) {
            m_probeStatus->setText(QStringLiteral("Пробные коды не сняты."));
        }
        return;
    }
    if (m_probeCodesPending) {
        return;
    }
    m_probeCodesOk = false;
    m_probeCodesPending = true;
    if (m_probeStatus) {
        m_probeStatus->setText(QStringLiteral("Съём пробных кодов…"));
    }
    if (m_probeOk) {
        m_probeOk->setEnabled(false);
    }
    const double power = m_power ? m_power->value() : -30.0;
    emit probeCodesRequested(m_fStartHz, m_fStopHz, m_points, m_ifbwHz, power, m_averages);
}

void StartWizard::onProbeCodesFinished(bool ok, const QString& message)
{
    m_probeCodesPending = false;
    m_probeCodesOk = ok;
    if (m_probeOk) {
        m_probeOk->setEnabled(true);
        if (!ok) {
            // Снять галочку, чтобы повторно запросить съём.
            QSignalBlocker blocker(m_probeOk);
            m_probeOk->setChecked(false);
        }
    }
    if (m_probeStatus) {
        m_probeStatus->setText(ok ? QStringLiteral("Успех: %1").arg(message)
                                  : QStringLiteral("Отказ: %1").arg(message));
    }
}

QString StartWizard::thruSummary() const
{
    if (!m_thruMag || !m_thruPhase) {
        return QStringLiteral("не измерено");
    }
    const QString metro = metrologistApproved()
        ? QStringLiteral("метролог: %1 (%2)")
              .arg(m_metroName->text().trimmed(),
                   m_metroDate->date().toString(Qt::ISODate))
        : QStringLiteral("без утверждения метролога");
    return QStringLiteral("%1 дБ / %2° (порог %3/%4); %5")
        .arg(m_thruMag->value(), 0, 'f', 2)
        .arg(m_thruPhase->value(), 0, 'f', 1)
        .arg(thruMagLimitDb(), 0, 'f', 2)
        .arg(thruPhaseLimitDeg(), 0, 'f', 1)
        .arg(metro);
}

bool StartWizard::fullSeriesVolume() const
{
    return m_seriesFull && m_seriesFull->isChecked();
}

bool StartWizard::metrologistApproved() const
{
    return m_metroApproved && m_metroApproved->isChecked()
        && m_metroName && !m_metroName->text().trimmed().isEmpty();
}

double StartWizard::thruMagLimitDb() const
{
    return m_thruMagLimit ? m_thruMagLimit->value() : 0.20;
}

double StartWizard::thruPhaseLimitDeg() const
{
    return m_thruPhaseLimit ? m_thruPhaseLimit->value() : 2.0;
}

void StartWizard::persistThruMeta()
{
    QSettings settings;
    if (m_thruMagLimit) {
        settings.setValue(QStringLiteral("ui/thru_mag_limit_db"), m_thruMagLimit->value());
    }
    if (m_thruPhaseLimit) {
        settings.setValue(QStringLiteral("ui/thru_phase_limit_deg"), m_thruPhaseLimit->value());
    }
    if (m_metroApproved) {
        settings.setValue(QStringLiteral("ui/metro_approved"), m_metroApproved->isChecked());
    }
    if (m_metroName) {
        settings.setValue(QStringLiteral("ui/metro_name"), m_metroName->text().trimmed());
    }
    if (m_metroDate) {
        settings.setValue(QStringLiteral("ui/metro_date"),
                          m_metroDate->date().toString(Qt::ISODate));
    }
}

bool StartWizard::confirmMetrologistGate()
{
    persistThruMeta();
    if (metrologistApproved()) {
        return true;
    }
    if (engineerProfile()) {
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("Без утверждения метролога"),
            QStringLiteral(
                "Утверждение метролога не отмечено. Инженерный профиль: "
                "можно продолжить с предупреждением (настройки ПО ≠ метрология).\n\n"
                "Продолжить подготовку серии?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        return answer == QMessageBox::Yes;
    }
    QMessageBox::warning(
        this, QStringLiteral("Серия не стартует"),
        QStringLiteral(
            "Без утверждения метролога обычная серия не готовится.\n"
            "Отметьте галку, ФИО и дату на шаге THRU, либо включите "
            "инженерный профиль на шаге 1 (с предупреждением)."));
    return false;
}

void StartWizard::writeThruApprovalJson(const QString& fixturesDir) const
{
    QFile f(QDir(fixturesDir).filePath(QStringLiteral("thru-approval.json")));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    const QString metroName =
        m_metroName ? m_metroName->text().trimmed().replace(QLatin1Char('"'), QLatin1Char('\''))
                    : QString();
    out << "{\n"
           "  \"thru_measured_mag_db\": "
        << (m_thruMag ? m_thruMag->value() : 0.0) << ",\n"
           "  \"thru_measured_phase_deg\": "
        << (m_thruPhase ? m_thruPhase->value() : 0.0) << ",\n"
           "  \"thru_limit_mag_db\": " << thruMagLimitDb() << ",\n"
           "  \"thru_limit_phase_deg\": " << thruPhaseLimitDeg() << ",\n"
           "  \"metrologist_approved\": "
        << (metrologistApproved() ? "true" : "false") << ",\n"
           "  \"metrologist_name\": \""
        << metroName << "\",\n"
           "  \"metrologist_date\": \""
        << (m_metroDate ? m_metroDate->date().toString(Qt::ISODate) : QString()) << "\",\n"
           "  \"note\": \"software limits, not accredited metrology\"\n"
           "}\n";
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
                                 QStringLiteral("Подтвердите идентификацию C2220 и контроллера."));
        return false;
    }
    if (id == 2) {
        if (!calChecklistComplete()) {
            QMessageBox::information(
                this, QStringLiteral("Шаг не завершён"),
                QStringLiteral("Отметьте все пункты чеклиста Response/Thru в S2VNA."));
            return false;
        }
        if (!m_calOk->isChecked()) {
            QMessageBox::information(
                this, QStringLiteral("Шаг не завершён"),
                QStringLiteral("Подтвердите, что калибровка выполнена в S2VNA."));
            return false;
        }
        persistVnaCalibrationId();
    }
    if (id == 3) {
        const double magLim = thruMagLimitDb();
        const double phLim = thruPhaseLimitDeg();
        if (m_thruMag->value() > magLim || m_thruPhase->value() > phLim) {
            QMessageBox::warning(
                this, QStringLiteral("THRU вне порога"),
                QStringLiteral("Значения превышают порог %1 дБ / %2°. "
                               "Скорректируйте измерение или пороги (настройки ПО).")
                    .arg(magLim, 0, 'f', 2)
                    .arg(phLim, 0, 'f', 1));
            return false;
        }
        if (m_metroApproved->isChecked() && m_metroName->text().trimmed().isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Утверждение метрологом"),
                QStringLiteral("Укажите ФИО метролога или снимите галку утверждения."));
            return false;
        }
        persistThruMeta();
    }
    if (id == 4 && !m_noOverload->isChecked()) {
        QMessageBox::information(this, QStringLiteral("Шаг не завершён"),
                                 QStringLiteral("Подтвердите отсутствие перегрузки."));
        return false;
    }
    if (id == 5 && (!m_probeOk->isChecked() || !m_probeCodesOk || m_probeCodesPending)) {
        QMessageBox::information(
            this, QStringLiteral("Шаг не завершён"),
            m_probeCodesPending
                ? QStringLiteral("Дождитесь окончания пробного съёма.")
                : QStringLiteral(
                      "Отметьте «Подтверждаю» и дождитесь успеха пробных кодов."));
        return false;
    }
    if (id == 6) {
        return confirmDangerousSettings();
    }
    if (id == 7) {
        return confirmMetrologistGate();
    }
    return true;
}

bool StartWizard::materializeSimFixtures(QString& diagnostics)
{
    persistVnaCalibrationId();
    persistThruMeta();
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
    writeThruApprovalJson(fixtures.path());

    const bool full = fullSeriesVolume();
    const int chFirst = 1;
    const int chLast = full ? 16 : 1;
    const int phFirst = 0;
    const int phLast = full ? 63 : 3;

    const double power = m_power->value();
    const qint64 fStart = static_cast<qint64>(m_fStartHz + 0.5);
    const qint64 fStop = static_cast<qint64>(m_fStopHz + 0.5);
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
           "    \"channels\": { \"first\": "
        << chFirst << ", \"last\": " << chLast
        << " },\n"
           "    \"phase_codes\": { \"first\": "
        << phFirst << ", \"last\": " << phLast
        << ", \"lsb_deg\": 5.625 },\n"
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
    csvOut << "att_code,att_cmd_db,enabled,settle_ms\n";
    if (full) {
        for (int i = 0; i < 64; ++i) {
            csvOut << i << ',' << QString::number(i * 0.5, 'f', 2) << ",true,0\n";
        }
    } else {
        csvOut << "0,0.00,true,0\n"
                  "1,0.50,true,0\n"
                  "2,1.00,false,0\n";
    }
    csv.close();
    return true;
}
