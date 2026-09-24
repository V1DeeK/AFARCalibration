#pragma once

#include <QVector>
#include <QWidget>

class QMouseEvent;
class QWheelEvent;

/// Мини-графики |S21| (дБ) и unwrap фазы (без Qt Charts, QPainter).
class S21PlotWidget final : public QWidget {
    Q_OBJECT

public:
    explicit S21PlotWidget(QWidget* parent = nullptr);

    void setTraceName(const QString& name);
    void setCurves(const QVector<double>& freqGhz,
                   const QVector<double>& magDb,
                   const QVector<double>& phaseUnwrapDeg);
    void clearCurves();
    [[nodiscard]] double visibleStartFraction() const noexcept { return m_viewLeft; }
    [[nodiscard]] double visibleSpanFraction() const noexcept { return m_viewRight - m_viewLeft; }

public slots:
    void resetView();

protected:
    void paintEvent(QPaintEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void paintPanel(QPainter& p,
                    const QRect& area,
                    const QString& title,
                    const QString& yUnit,
                    const QVector<double>& y) const;

    QVector<double> m_freqGhz;
    QVector<double> m_magDb;
    QVector<double> m_phaseDeg;
    QString m_traceName{QStringLiteral("S21")};
    double m_viewLeft = 0.0;
    double m_viewRight = 1.0;
    bool m_dragging = false;
    double m_lastDragX = 0.0;
};
