#include "S21PlotWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QSizePolicy>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kGridDivs = 5; // 4–6 делений по осям

} // namespace

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
    m_overlayFreqGhz.clear();
    m_overlayY.clear();
    update();
}

void S21PlotWidget::setPanelTitles(const QString& magTitle, const QString& phaseTitle)
{
    m_magTitle = magTitle;
    m_phaseTitle = phaseTitle;
    update();
}

void S21PlotWidget::setEmptyHint(const QString& hint)
{
    m_emptyHint = hint;
    update();
}

void S21PlotWidget::setSubtitle(const QString& subtitle)
{
    m_subtitle = subtitle;
    update();
}

void S21PlotWidget::setOverlayCurves(const QVector<double>& freqGhz, const QVector<double>& y)
{
    m_overlayFreqGhz = freqGhz;
    m_overlayY = y;
    update();
}

void S21PlotWidget::paintPanel(QPainter& p,
                               const QRect& area,
                               const QString& title,
                               const QVector<double>& y,
                               const QVector<double>& overlayFreq,
                               const QVector<double>& overlayY) const
{
    const QPalette pal = palette();
    p.fillRect(area, pal.color(QPalette::Base));
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    p.drawRect(area.adjusted(0, 0, -1, -1));

    const QRect plot = area.adjusted(44, 18, -8, -22);
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(area.adjusted(6, 2, -6, 0), Qt::AlignLeft | Qt::AlignTop, title);

    if (y.size() < 2 || m_freqGhz.size() != y.size() || plot.width() < 4 || plot.height() < 4) {
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(plot, Qt::AlignCenter | Qt::TextWordWrap, m_emptyHint);
        return;
    }

    double yMin = y[0];
    double yMax = y[0];
    auto expandY = [&](const QVector<double>& vals) {
        for (double v : vals) {
            if (!std::isfinite(v)) {
                continue;
            }
            yMin = std::min(yMin, v);
            yMax = std::max(yMax, v);
        }
    };
    expandY(y);
    const bool drawOverlay = overlayY.size() >= 2 && overlayFreq.size() == overlayY.size();
    if (drawOverlay) {
        expandY(overlayY);
    }
    if (!std::isfinite(yMin) || !std::isfinite(yMax) || yMax <= yMin) {
        yMin = 0.0;
        yMax = 1.0;
    } else {
        const double pad = (yMax - yMin) * 0.08;
        yMin -= pad;
        yMax += pad;
    }

    const double x0 = m_freqGhz.first();
    const double x1 = m_freqGhz.last();
    const double dx = (x1 > x0) ? (x1 - x0) : 1.0;
    const double dy = yMax - yMin;

    // Сетка и подписи осей (без antialiasing).
    QColor gridColor = pal.color(QPalette::Mid);
    gridColor.setAlpha(90);
    const QFontMetrics fm(p.font());
    for (int i = 0; i <= kGridDivs; ++i) {
        const double t = static_cast<double>(i) / kGridDivs;
        const int x = plot.left() + static_cast<int>(t * plot.width());
        const int yPix = plot.bottom() - static_cast<int>(t * plot.height());

        p.setPen(QPen(gridColor, 1, Qt::DotLine));
        p.drawLine(plot.left(), yPix, plot.right(), yPix);
        p.drawLine(x, plot.top(), x, plot.bottom());

        const double yVal = yMin + t * dy;
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(QRect(area.left() + 2, yPix - 7, 40, 14),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(yVal, 'f', 1));

        const double xVal = x0 + t * dx;
        const QString xLabel = QString::number(xVal, 'f', 2);
        const int tw = fm.horizontalAdvance(xLabel);
        int labelLeft = x - tw / 2;
        if (i == 0) {
            labelLeft = plot.left();
        } else if (i == kGridDivs) {
            labelLeft = plot.right() - tw;
        }
        p.drawText(QRect(labelLeft, plot.bottom() + 2, tw + 2, 16),
                   Qt::AlignLeft | Qt::AlignTop, xLabel);
    }

    auto mapPoly = [&](const QVector<double>& fx, const QVector<double>& fy) {
        QPolygonF poly;
        poly.reserve(fy.size());
        for (int i = 0; i < fy.size(); ++i) {
            const double xv = fx[i];
            const double yv = std::isfinite(fy[i]) ? fy[i] : yMin;
            const double nx = (xv - x0) / dx;
            const double ny = (yv - yMin) / dy;
            poly << QPointF(plot.left() + nx * plot.width(),
                            plot.bottom() - ny * plot.height());
        }
        return poly;
    };

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(pal.color(QPalette::Highlight), 1.5));
    p.drawPolyline(mapPoly(m_freqGhz, y));
    if (drawOverlay) {
        p.setPen(QPen(QColor(0xC0, 0x55, 0x20), 1.5));
        p.drawPolyline(mapPoly(overlayFreq, overlayY));
    }
    p.setRenderHint(QPainter::Antialiasing, false);
}

void S21PlotWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    QRect r = rect().adjusted(1, 1, -1, -1);

    if (!m_subtitle.isEmpty()) {
        p.setPen(palette().color(QPalette::Mid));
        p.drawText(r.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignTop, m_subtitle);
        r.setTop(r.top() + 16);
    }

    const int mid = r.top() + r.height() / 2;
    const QRect top(r.left(), r.top(), r.width(), mid - r.top() - 2);
    const QRect bottom(r.left(), mid + 2, r.width(), r.bottom() - mid - 2);
    paintPanel(p, top, m_magTitle, m_magDb, {}, {});
    paintPanel(p, bottom, m_phaseTitle, m_phaseDeg, m_overlayFreqGhz, m_overlayY);
}
