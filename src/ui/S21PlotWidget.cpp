#include "S21PlotWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QSizePolicy>
#include <algorithm>
#include <cmath>

S21PlotWidget::S21PlotWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void S21PlotWidget::setCurves(const QVector<double>& freqGhz,
                              const QVector<double>& magDb,
                              const QVector<double>& phaseUnwrapDeg)
{
    m_freqGhz = freqGhz;
    m_magDb = magDb;
    m_phaseDeg = phaseUnwrapDeg;
    update();
}

void S21PlotWidget::clearCurves()
{
    m_freqGhz.clear();
    m_magDb.clear();
    m_phaseDeg.clear();
    update();
}

void S21PlotWidget::paintPanel(QPainter& p,
                               const QRect& area,
                               const QString& title,
                               const QString& yUnit,
                               const QVector<double>& y) const
{
    const QPalette pal = palette();
    p.fillRect(area, pal.color(QPalette::Base));
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    p.drawRect(area.adjusted(0, 0, -1, -1));

    const QRect plot = area.adjusted(44, 18, -8, -16);
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(area.adjusted(6, 2, -6, 0), Qt::AlignLeft | Qt::AlignTop, title);

    if (y.size() < 2 || m_freqGhz.size() != y.size() || plot.width() < 4 || plot.height() < 4) {
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(plot, Qt::AlignCenter,
                   QStringLiteral("Нет данных свипа"));
        return;
    }

    double yMin = y[0];
    double yMax = y[0];
    for (double v : y) {
        if (!std::isfinite(v)) {
            continue;
        }
        yMin = std::min(yMin, v);
        yMax = std::max(yMax, v);
    }
    if (!std::isfinite(yMin) || !std::isfinite(yMax) || yMax <= yMin) {
        yMin = 0.0;
        yMax = 1.0;
    } else {
        const double pad = (yMax - yMin) * 0.08;
        yMin -= pad;
        yMax += pad;
    }

    p.setPen(pal.color(QPalette::Mid));
    p.drawText(QRect(area.left() + 2, plot.top(), 40, 14), Qt::AlignRight | Qt::AlignVCenter,
               QString::number(yMax, 'f', 1));
    p.drawText(QRect(area.left() + 2, plot.bottom() - 14, 40, 14),
               Qt::AlignRight | Qt::AlignVCenter, QString::number(yMin, 'f', 1));
    p.drawText(QRect(area.left() + 2, plot.center().y() - 7, 40, 14),
               Qt::AlignRight | Qt::AlignVCenter, yUnit);

    const double x0 = m_freqGhz.first();
    const double x1 = m_freqGhz.last();
    const double dx = (x1 > x0) ? (x1 - x0) : 1.0;

    QPolygonF poly;
    poly.reserve(y.size());
    for (int i = 0; i < y.size(); ++i) {
        const double xv = m_freqGhz[i];
        const double yv = std::isfinite(y[i]) ? y[i] : yMin;
        const double nx = (xv - x0) / dx;
        const double ny = (yv - yMin) / (yMax - yMin);
        poly << QPointF(plot.left() + nx * plot.width(),
                        plot.bottom() - ny * plot.height());
    }

    p.setPen(QPen(pal.color(QPalette::Highlight), 1.5));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawPolyline(poly);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void S21PlotWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    const QRect r = rect().adjusted(1, 1, -1, -1);
    const int mid = r.top() + r.height() / 2;
    const QRect top(r.left(), r.top(), r.width(), mid - r.top() - 2);
    const QRect bottom(r.left(), mid + 2, r.width(), r.bottom() - mid - 2);
    paintPanel(p, top, QStringLiteral("|S21|"), QStringLiteral("дБ"), m_magDb);
    paintPanel(p, bottom, QStringLiteral("Фаза (unwrap)"), QStringLiteral("°"), m_phaseDeg);
}
