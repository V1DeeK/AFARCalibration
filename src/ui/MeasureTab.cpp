#include "MeasureTab.h"

#include "S21PlotWidget.h"

#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>

MeasureTab::MeasureTab(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);

    m_stages = new QListWidget(this);
    const QStringList stages = {
        QStringLiteral("1 Подключения"),
        QStringLiteral("2 Калибровка C2220"),
        QStringLiteral("3 Линейность"),
        QStringLiteral("4 Перебор кодов"),
        QStringLiteral("5 Прямая LUT"),
        QStringLiteral("6 Обратная LUT"),
        QStringLiteral("7 Валидация"),
    };
    m_stages->addItems(stages);
    m_stages->setCurrentRow(0);
    m_stages->setFixedWidth(200);
    connect(m_stages, &QListWidget::currentRowChanged, this, &MeasureTab::onStageClicked);

    m_stack = new QStackedWidget(this);
    m_stack->addWidget(makeConnectionsPage());
    m_stack->addWidget(makeCalPage());
    m_stack->addWidget(makeLinearityPage());
    m_stack->addWidget(makeSweepPage());
    m_stack->addWidget(makeDirectPage());
    m_stack->addWidget(makeInversePage());
    m_stack->addWidget(makeValidationPage());

    root->addWidget(m_stages);
    root->addWidget(m_stack, 1);

    applyRunConfigDefaults(4.9, 6.0, 201, 1000, -30.0, 8);
    setStandCheck(false, false, false, QStringLiteral("не измерено"), false, false, -30.0, false);
    setSeriesArtifacts(QStringLiteral("—"), QStringLiteral("direct-lut.parquet"),
                       QStringLiteral("inverse-lut.parquet"), QStringLiteral("report.pdf"),
                       QStringLiteral("manifest.sha256"));
}

QWidget* MeasureTab::makeConnectionsPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);

    auto* title = new QLabel(QStringLiteral("Этап 1. Подключения и мастер запуска"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));

    m_connectHow = new QLabel(
        QStringLiteral(
            "Как запустить серию на имитаторах:\n"
            "1. Нажмите синюю кнопку «Мастер запуска» внизу окна или кнопку ниже.\n"
            "2. На каждом шаге отметьте «Подтверждаю» и жмите «Далее».\n"
            "3. На последнем шаге — «Готово»: оркестратор дойдёт до READY.\n"
            "4. Зелёная «Старт» начнёт перебор. Жёлтая «Пауза», красная «Стоп» "
            "(Стоп спросит подтверждение).\n"
            "Частоты и точки свипа берутся из этапа «4 Перебор кодов»."),
        page);
    m_connectHow->setWordWrap(true);
    m_connectHow->setObjectName(QStringLiteral("hintLabel"));

    auto* openWizard = new QPushButton(QStringLiteral("Открыть мастер запуска"), page);
    openWizard->setObjectName(QStringLiteral("btnPrimary"));
    connect(openWizard, &QPushButton::clicked, this, &MeasureTab::openWizardRequested);

    auto* resume = new QPushButton(QStringLiteral("Продолжить незакрытую серию"), page);
    resume->setObjectName(QStringLiteral("btnResume"));
    connect(resume, &QPushButton::clicked, this, &MeasureTab::resumeSeriesRequested);

    m_unfinished = new QLabel(QStringLiteral("Незакрытая серия не найдена."), page);
    m_unfinished->setWordWrap(true);
    m_unfinished->setObjectName(QStringLiteral("hintLabel"));

    layout->addWidget(title);
    layout->addWidget(m_connectHow);
    layout->addWidget(openWizard);
    layout->addWidget(resume);
    layout->addWidget(m_unfinished);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeCalPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 2. Калибровка C2220"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_calText = new QLabel(page);
    m_calText->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(m_calText);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeLinearityPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 3. Линейность и перегрузка"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_linText = new QLabel(page);
    m_linText->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(m_linText);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeSweepPage()
{
    auto* page = new QWidget(this);
    auto* center = new QVBoxLayout(page);

    auto* params = new QGroupBox(QStringLiteral("Параметры свипа"), page);
    auto* form = new QFormLayout(params);
    m_fStart = new QDoubleSpinBox(params);
    m_fStart->setRange(0.1, 40.0);
    m_fStart->setDecimals(3);
    m_fStart->setSuffix(QStringLiteral(" ГГц"));
    m_fStop = new QDoubleSpinBox(params);
    m_fStop->setRange(0.1, 40.0);
    m_fStop->setDecimals(3);
    m_fStop->setSuffix(QStringLiteral(" ГГц"));
    m_points = new QSpinBox(params);
    m_points->setRange(2, 10001);
    m_ifbw = new QSpinBox(params);
    m_ifbw->setRange(1, 1000000);
    m_ifbw->setSuffix(QStringLiteral(" Гц"));
    m_power = new QDoubleSpinBox(params);
    m_power->setRange(-60.0, 10.0);
    m_power->setDecimals(1);
    m_power->setSuffix(QStringLiteral(" дБм"));
    m_averages = new QSpinBox(params);
    m_averages->setRange(1, 1024);
    form->addRow(QStringLiteral("f нач."), m_fStart);
    form->addRow(QStringLiteral("f кон."), m_fStop);
    form->addRow(QStringLiteral("Точки"), m_points);
    form->addRow(QStringLiteral("ПЧ"), m_ifbw);
    form->addRow(QStringLiteral("Мощность"), m_power);
    form->addRow(QStringLiteral("Усреднение"), m_averages);

    m_current = new QLabel(QStringLiteral("Канал: —  Att: —  Фаза: —"), page);
    m_plot = new S21PlotWidget(page);

    m_progress = new QProgressBar(page);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_counter = new QLabel(QStringLiteral("Состояния: 0 / 0"), page);
    m_eta = new QLabel(QStringLiteral("ETA: —"), page);

    center->addWidget(params);
    center->addWidget(m_current);
    center->addWidget(m_plot, 1);
    center->addWidget(m_progress);
    center->addWidget(m_counter);
    center->addWidget(m_eta);
    return page;
}

QWidget* MeasureTab::makeDirectPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 5. Прямая LUT (FR-13)"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_directText = new QLabel(page);
    m_directText->setWordWrap(true);
    m_directText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    layout->addWidget(m_directText);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeInversePage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 6. Обратная LUT (FR-14)"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_inverseText = new QLabel(page);
    m_inverseText->setWordWrap(true);
    m_inverseText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    layout->addWidget(m_inverseText);
    layout->addStretch(1);
    return page;
}

QWidget* MeasureTab::makeValidationPage()
{
    auto* page = new QWidget(this);
    auto* layout = new QVBoxLayout(page);
    auto* title = new QLabel(QStringLiteral("Этап 7. Валидация и манифест"), page);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 700;"));
    m_validText = new QLabel(page);
    m_validText->setWordWrap(true);
    m_validText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(title);
    layout->addWidget(m_validText);
    layout->addStretch(1);
    return page;
}

void MeasureTab::onStageClicked(int row)
{
    if (row >= 0 && row < m_stack->count()) {
        m_stack->setCurrentIndex(row);
    }
}

void MeasureTab::applyRunConfigDefaults(double fStartGhz,
                                        double fStopGhz,
                                        int points,
                                        int ifbwHz,
                                        double powerDbm,
                                        int averages)
{
    m_fStart->setValue(fStartGhz);
    m_fStop->setValue(fStopGhz);
    m_points->setValue(points);
    m_ifbw->setValue(ifbwHz);
    m_power->setValue(powerDbm);
    m_averages->setValue(averages);
}

void MeasureTab::setStageHighlight(int stageIndex0)
{
    if (stageIndex0 >= 0 && stageIndex0 < m_stages->count()) {
        m_stages->setCurrentRow(stageIndex0);
    }
}

void MeasureTab::setProgress(qint64 completed,
                             qint64 total,
                             int channel,
                             int attCode,
                             int phaseCode)
{
    if (total <= 0) {
        m_progress->setValue(0);
    } else {
        m_progress->setValue(static_cast<int>((completed * 100) / total));
    }
    m_counter->setText(QStringLiteral("Состояния: %1 / %2").arg(completed).arg(total));
    m_current->setText(QStringLiteral("Канал: %1  Att: %2  Фаза: %3")
                           .arg(channel)
                           .arg(attCode)
                           .arg(phaseCode));
}

void MeasureTab::setEtaText(const QString& text)
{
    m_eta->setText(text);
}

void MeasureTab::setSweepCurves(const QVector<double>& freqGhz,
                                const QVector<double>& magDb,
                                const QVector<double>& phaseUnwrapDeg)
{
    m_plot->setCurves(freqGhz, magDb, phaseUnwrapDeg);
}

void MeasureTab::setStandCheck(bool connectionsOk,
                               bool idnOk,
                               bool calOk,
                               const QString& thruText,
                               bool noOverload,
                               bool probeOk,
                               double powerDbm,
                               bool engineer)
{
    m_calText->setText(
        QStringLiteral(
            "Калибровка ВАЦ в этапе 1 — проверка флага/идентификатора профиля "
            "(не замена метрологической аттестации).\n\n"
            "Подключения: %1\n"
            "IDN C2220 / контроллер: %2\n"
            "Флаг калибровки: %3\n"
            "THRU: %4\n"
            "Пороги по умолчанию FR-05: 0,20 дБ / 2,0° — конфигурируются в мастере.")
            .arg(connectionsOk ? QStringLiteral("подтверждены") : QStringLiteral("ожидают мастера"),
                 idnOk ? QStringLiteral("подтверждены") : QStringLiteral("ожидают мастера"),
                 calOk ? QStringLiteral("действительна (флаг профиля)")
                       : QStringLiteral("не подтверждена"),
                 thruText));

    m_linText->setText(
        QStringLiteral(
            "Линейность на этапе 1 — контроль мощности и отсутствия перегрузки приёмника "
            "до полного перебора кодов.\n\n"
            "Мощность: %1 дБм\n"
            "Перегрузка: %2\n"
            "Пробные коды: %3\n"
            "Инженерный профиль: %4\n\n"
            "Опасные мощность / direct access / safe_state доступны только инженеру (UI-06).")
            .arg(powerDbm, 0, 'f', 1)
            .arg(noOverload ? QStringLiteral("не обнаружена (подтверждено)")
                            : QStringLiteral("ещё не подтверждена"))
            .arg(probeOk ? QStringLiteral("разрешены") : QStringLiteral("ожидают мастера"))
            .arg(engineer ? QStringLiteral("включён") : QStringLiteral("выключен")));
}

void MeasureTab::setSeriesArtifacts(const QString& seriesRoot,
                                    const QString& directLut,
                                    const QString& inverseLut,
                                    const QString& report,
                                    const QString& manifest)
{
    m_directText->setText(
        QStringLiteral(
            "Прямая LUT пишется при Finalizing → Complete: измеренные комплексные S21, "
            "модуль, развёрнутая фаза, дрейф, повторяемость, флаг valid (FR-13).\n\n"
            "Серия: %1\n"
            "Файл: %2\n\n"
            "Пока серия не завершена, файла ещё нет.")
            .arg(seriesRoot, directLut));
    m_inverseText->setText(
        QStringLiteral(
            "Обратная LUT подбирает пару кодов под цель по минимуму J "
            "(веса ослабления и фазы, штраф invalid) — FR-14, без допущения монотонности.\n\n"
            "Серия: %1\n"
            "Файл: %2")
            .arg(seriesRoot, inverseLut));
    m_validText->setText(
        QStringLiteral(
            "Валидация закрывает серию: QC (FR-16/17), PDF-протокол и manifest.sha256 (FR-09).\n\n"
            "Стартовые пороги (не метрология изделия): дрейф фазы 1,0°; "
            "остаток обратной LUT 2,8125°; повторяемость 0,10 дБ / 1,0°.\n\n"
            "Серия: %1\n"
            "Протокол: %2\n"
            "Манифест: %3")
            .arg(seriesRoot, report, manifest));
}

void MeasureTab::setUnfinishedSeriesHint(const QString& path)
{
    if (path.isEmpty()) {
        m_unfinished->setText(QStringLiteral("Незакрытая серия не найдена."));
    } else {
        m_unfinished->setText(
            QStringLiteral("Найдена незакрытая серия (нет report.pdf):\n%1\n"
                           "Нажмите «Продолжить незакрытую серию» (FR-12).")
                .arg(path));
    }
}

double MeasureTab::fStartGhz() const
{
    return m_fStart->value();
}

double MeasureTab::fStopGhz() const
{
    return m_fStop->value();
}

int MeasureTab::points() const
{
    return m_points->value();
}

int MeasureTab::ifbwHz() const
{
    return m_ifbw->value();
}

double MeasureTab::powerDbm() const
{
    return m_power->value();
}

int MeasureTab::averages() const
{
    return m_averages->value();
}
