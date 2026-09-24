#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

/// Мини-графики |S21| (дБ) и unwrap фазы (без Qt Charts, QPainter).
class S21PlotWidget final : public QWidget {
    Q_OBJECT

public:
    explicit S21PlotWidget(QWidget* parent = nullptr);

    void setCurves(const QVector<double>& freqGhz,
                   const QVector<double>& magDb,
                   const QVector<double>& phaseUnwrapDeg);
    void clearCurves();

    void setPanelTitles(const QString& magTitle, const QString& phaseTitle);
    void setEmptyHint(const QString& hint);
    void setSubtitle(const QString& subtitle);

    /// Вторая линия (например residual) на нижней панели; пустой y — не рисуется.
    void setOverlayCurves(const QVector<double>& freqGhz, const QVector<double>& y);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintPanel(QPainter& p,
                    const QRect& area,
                    const QString& title,
                    const QVector<double>& y,
                    const QVector<double>& overlayFreq,
                    const QVector<double>& overlayY) const;

    QVector<double> m_freqGhz;
    QVector<double> m_magDb;
    QVector<double> m_phaseDeg;
    QVector<double> m_overlayFreqGhz;
    QVector<double> m_overlayY;

    QString m_magTitle = QStringLiteral("|S21| (модуль, дБ)");
    QString m_phaseTitle = QStringLiteral("фаза unwrap (°)");
    QString m_emptyHint =
        QStringLiteral("Нет данных свипа — нажмите Старт или выберите ячейку матрицы");
    QString m_subtitle;
};
