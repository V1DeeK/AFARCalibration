#include "DataFormatsTab.h"

#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

DataFormatsTab::DataFormatsTab(QWidget* parent)
    : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    root->addWidget(new QLabel(QStringLiteral("Пути каталога серии (SeriesDirectory):"), this));

    auto* form = new QFormLayout();
    auto makePath = [this](const QString& placeholder) {
        auto* lab = new QLabel(placeholder, this);
        lab->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lab->setWordWrap(true);
        return lab;
    };
    m_root = makePath(QStringLiteral("—"));
    m_runConfig = makePath(QStringLiteral("run-config.json"));
    m_att = makePath(QStringLiteral("attenuator-codes.csv"));
    m_raw = makePath(QStringLiteral("raw-s21.h5"));
    m_direct = makePath(QStringLiteral("direct-lut.parquet"));
    m_inverse = makePath(QStringLiteral("inverse-lut.parquet"));
    m_report = makePath(QStringLiteral("report.pdf"));
    m_manifest = makePath(QStringLiteral("manifest.sha256"));
    m_events = makePath(QStringLiteral("run-events.jsonl"));

    form->addRow(QStringLiteral("Корень серии"), m_root);
    form->addRow(QStringLiteral("Конфиг"), m_runConfig);
    form->addRow(QStringLiteral("Коды att"), m_att);
    form->addRow(QStringLiteral("Raw HDF5"), m_raw);
    form->addRow(QStringLiteral("Прямая LUT"), m_direct);
    form->addRow(QStringLiteral("Обратная LUT"), m_inverse);
    form->addRow(QStringLiteral("Протокол"), m_report);
    form->addRow(QStringLiteral("Манифест"), m_manifest);
    form->addRow(QStringLiteral("Журнал"), m_events);
    root->addLayout(form);
    root->addStretch(1);
}

void DataFormatsTab::setSeriesPaths(const QString& seriesRoot,
                                    const QString& runConfig,
                                    const QString& attenuatorCsv,
                                    const QString& rawS21,
                                    const QString& directLut,
                                    const QString& inverseLut,
                                    const QString& report,
                                    const QString& manifest,
                                    const QString& runEvents)
{
    m_root->setText(seriesRoot);
    m_runConfig->setText(runConfig);
    m_att->setText(attenuatorCsv);
    m_raw->setText(rawS21);
    m_direct->setText(directLut);
    m_inverse->setText(inverseLut);
    m_report->setText(report);
    m_manifest->setText(manifest);
    m_events->setText(runEvents);
}

void DataFormatsTab::clearPaths()
{
    setSeriesPaths(QStringLiteral("—"), QStringLiteral("run-config.json"),
                   QStringLiteral("attenuator-codes.csv"), QStringLiteral("raw-s21.h5"),
                   QStringLiteral("direct-lut.parquet"), QStringLiteral("inverse-lut.parquet"),
                   QStringLiteral("report.pdf"), QStringLiteral("manifest.sha256"),
                   QStringLiteral("run-events.jsonl"));
}
