#pragma once

#include <QColor>
#include <QString>
#include <QVector>
#include <QWidget>

class QMouseEvent;
class QPainter;
class QWheelEvent;

struct S21PlotTrace {
    QString name;
    QVector<double> freqGhz;
    QVector<double> magDb;
    QVector<double> phaseDeg;
    QColor color;
};

/// Мини-графики |S21| (дБ) и unwrap фазы (без Qt Charts, QPainter).
class S21PlotWidget final : public QWidget {
    Q_OBJECT

public:
    explicit S21PlotWidget(QWidget* parent = nullptr);

    void setCurves(const QVector<double>& freqGhz,
                   const QVector<double>& magDb,
                   const QVector<double>& phaseUnwrapDeg);
    void setTraces(const QVector<S21PlotTrace>& traces);
    void clearCurves();

    void setPanelTitles(const QString& magTitle, const QString& phaseTitle);
    void setSinglePanelMode(bool enabled);
    void setEmptyHint(const QString& hint);
    void setSubtitle(const QString& subtitle);

    /// Вторая линия (например residual) на нижней панели; пустой y — не рисуется.
    void setOverlayCurves(const QVector<double>& freqGhz, const QVector<double>& y);
    void setMarkerFrequencies(const QVector<double>& freqGhz);
    void setMarkerPlacementEnabled(bool enabled);

    [[nodiscard]] double visibleStartFraction() const noexcept { return m_viewLeft; }
    [[nodiscard]] double visibleSpanFraction() const noexcept { return m_viewRight - m_viewLeft; }
    [[nodiscard]] int traceCount() const noexcept { return m_traces.size(); }
    [[nodiscard]] qsizetype primaryPointCount() const noexcept;

signals:
    void markerRequested(double freqGhz);

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
                    bool phasePanel) const;
    [[nodiscard]] const S21PlotTrace* primaryTrace() const;

    QVector<S21PlotTrace> m_traces;
    QVector<double> m_overlayFreqGhz;
    QVector<double> m_overlayY;
    QVector<double> m_markerFreqGhz;

    QString m_magTitle = QStringLiteral("|S21| (модуль, дБ)");
    QString m_phaseTitle = QStringLiteral("фаза unwrap (°)");
    QString m_emptyHint =
        QStringLiteral("Нет данных свипа — нажмите Старт или выберите ячейку матрицы");
    QString m_subtitle;

    double m_viewLeft = 0.0;
    double m_viewRight = 1.0;
    bool m_dragging = false;
    bool m_markerPlacementEnabled = false;
    bool m_singlePanel = false;
    double m_lastDragX = 0.0;
};
