#pragma once

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

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void paintPanel(QPainter& p,
                    const QRect& area,
                    const QString& title,
                    const QString& yUnit,
                    const QVector<double>& y) const;

    QVector<double> m_freqGhz;
    QVector<double> m_magDb;
    QVector<double> m_phaseDeg;
};
