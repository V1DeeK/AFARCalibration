#pragma once

#include <QWidget>

class QLabel;

/// Пути серии по SeriesDirectory (UI-04 / UI-006).
class DataFormatsTab final : public QWidget {
    Q_OBJECT

public:
    explicit DataFormatsTab(QWidget* parent = nullptr);

    void setSeriesPaths(const QString& seriesRoot,
                        const QString& runConfig,
                        const QString& attenuatorCsv,
                        const QString& rawS21,
                        const QString& directLut,
                        const QString& inverseLut,
                        const QString& report,
                        const QString& manifest,
                        const QString& runEvents);
    void clearPaths();

private:
    QLabel* m_root = nullptr;
    QLabel* m_runConfig = nullptr;
    QLabel* m_att = nullptr;
    QLabel* m_raw = nullptr;
    QLabel* m_direct = nullptr;
    QLabel* m_inverse = nullptr;
    QLabel* m_report = nullptr;
    QLabel* m_manifest = nullptr;
    QLabel* m_events = nullptr;
};
