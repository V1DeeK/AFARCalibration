#include "S21PlotWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSizePolicy>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kGridDivs = 5; // 4–6 делений по осям
constexpr int kPlotLeftPad = 44;
constexpr int kPlotRightPad = 8;

} // namespace

S21PlotWidget::S21PlotWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setToolTip(QStringLiteral(
        "Колесо мыши — приблизить/отдалить; зажать левую кнопку — прокрутить; "
        "двойной щелчок — показать весь диапазон"));
}

void S21PlotWidget::setCurves(const QVector<double>& freqGhz,
                              const QVector<double>& magDb,
                              const QVector<double>& phaseUnwrapDeg)
{
    m_freqGhz = freqGhz;
    m_magDb = magDb;
    m_phaseDeg = phaseUnwrapDeg;
    resetView();
}

void S21PlotWidget::resetView()
{
    m_viewLeft = 0.0;
    m_viewRight = 1.0;
    m_dragging = false;
    unsetCursor();
    update();
}

void S21PlotWidget::clearCurves()
{
    m_freqGhz.clear();
    m_magDb.clear();
    m_phaseDeg.clear();
    m_overlayFreqGhz.clear();
    m_overlayY.clear();
    resetView();
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

    const QRect plot = area.adjusted(kPlotLeftPad, 18, -kPlotRightPad, -22);
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(area.adjusted(6, 2, -6, 0), Qt::AlignLeft | Qt::AlignTop, title);

    if (y.size() < 2 || m_freqGhz.size() != y.size() || plot.width() < 4 || plot.height() < 4) {
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(plot, Qt::AlignCenter | Qt::TextWordWrap, m_emptyHint);
        return;
    }

    const double fullX0 = m_freqGhz.first();
    const double fullX1 = m_freqGhz.last();
    const double fullDx = (fullX1 > fullX0) ? (fullX1 - fullX0) : 1.0;
    const double x0 = fullX0 + m_viewLeft * fullDx;
    const double x1 = fullX0 + m_viewRight * fullDx;
    const double dx = (x1 > x0) ? (x1 - x0) : 1.0;

    auto first = std::lower_bound(m_freqGhz.cbegin(), m_freqGhz.cend(), x0);
    auto last = std::upper_bound(m_freqGhz.cbegin(), m_freqGhz.cend(), x1);
    if (first != m_freqGhz.cbegin()) {
        --first;
    }
    if (last != m_freqGhz.cend()) {
        ++last;
    }
    const qsizetype firstIndex = std::distance(m_freqGhz.cbegin(), first);
    const qsizetype lastIndex = std::distance(m_freqGhz.cbegin(), last);

    double yMin = 0.0;
    double yMax = 0.0;
    bool haveFinite = false;
    auto expandYRange = [&](const QVector<double>& vals, qsizetype from, qsizetype to) {
        for (qsizetype i = from; i < to; ++i) {
            const double v = vals[i];
            if (!std::isfinite(v)) {
                continue;
            }
            if (!haveFinite) {
                yMin = v;
                yMax = v;
                haveFinite = true;
            } else {
                yMin = std::min(yMin, v);
                yMax = std::max(yMax, v);
            }
        }
    };
    expandYRange(y, firstIndex, lastIndex);

    const bool drawOverlay = overlayY.size() >= 2 && overlayFreq.size() == overlayY.size();
    if (drawOverlay) {
        for (double v : overlayY) {
            if (!std::isfinite(v)) {
                continue;
            }
            if (!haveFinite) {
                yMin = v;
                yMax = v;
                haveFinite = true;
            } else {
                yMin = std::min(yMin, v);
                yMax = std::max(yMax, v);
            }
        }
    }
    if (!haveFinite || !std::isfinite(yMin) || !std::isfinite(yMax) || yMax <= yMin) {
        yMin = 0.0;
        yMax = 1.0;
    } else {
        const double pad = (yMax - yMin) * 0.08;
        yMin -= pad;
        yMax += pad;
    }

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

    auto mapPoly = [&](const QVector<double>& fx, const QVector<double>& fy,
                       qsizetype from, qsizetype to) {
        QPolygonF poly;
        poly.reserve(to - from);
        for (qsizetype i = from; i < to; ++i) {
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
    p.drawPolyline(mapPoly(m_freqGhz, y, firstIndex, lastIndex));
    if (drawOverlay) {
        p.setPen(QPen(QColor(0xC0, 0x55, 0x20), 1.5));
        p.drawPolyline(mapPoly(overlayFreq, overlayY, 0, overlayY.size()));
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

void S21PlotWidget::wheelEvent(QWheelEvent* event)
{
    if (m_freqGhz.size() < 2 || event->angleDelta().y() == 0) {
        event->ignore();
        return;
    }
    const double width = std::max(1, this->width() - (kPlotLeftPad + kPlotRightPad + 1));
    const double cursor =
        std::clamp((event->position().x() - kPlotLeftPad) / width, 0.0, 1.0);
    const double oldSpan = m_viewRight - m_viewLeft;
    const double factor = std::pow(0.8, event->angleDelta().y() / 120.0);
    const double minSpan = std::max(1.0 / static_cast<double>(m_freqGhz.size() - 1), 0.002);
    const double newSpan = std::clamp(oldSpan * factor, minSpan, 1.0);
    const double anchor = m_viewLeft + cursor * oldSpan;
    m_viewLeft = std::clamp(anchor - cursor * newSpan, 0.0, 1.0 - newSpan);
    m_viewRight = m_viewLeft + newSpan;
    update();
    event->accept();
}

void S21PlotWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_viewRight - m_viewLeft < 0.999999) {
        m_dragging = true;
        m_lastDragX = event->position().x();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void S21PlotWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const double span = m_viewRight - m_viewLeft;
    const double shift = -(event->position().x() - m_lastDragX)
        / std::max(1, width() - (kPlotLeftPad + kPlotRightPad + 1)) * span;
    m_viewLeft = std::clamp(m_viewLeft + shift, 0.0, 1.0 - span);
    m_viewRight = m_viewLeft + span;
    m_lastDragX = event->position().x();
    update();
    event->accept();
}

void S21PlotWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void S21PlotWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        resetView();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
