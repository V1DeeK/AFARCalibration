#include "S21PlotWidget.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QSizePolicy>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kXMajorDivs = 8;
constexpr int kMinorDivs = 5;
constexpr int kPlotLeftPad = 62;
constexpr int kPlotRightPad = 16;
constexpr int kPlotTopPad = 28;
constexpr int kPlotBottomPad = 36;

double niceStep(double rawStep)
{
    if (!std::isfinite(rawStep) || rawStep <= 0.0) {
        return 1.0;
    }
    const double magnitude = std::pow(10.0, std::floor(std::log10(rawStep)));
    const double fraction = rawStep / magnitude;
    const double niceFraction = fraction <= 1.0 ? 1.0
        : (fraction <= 2.0 ? 2.0 : (fraction <= 5.0 ? 5.0 : 10.0));
    return niceFraction * magnitude;
}

int decimalsForStep(double step)
{
    return std::clamp(static_cast<int>(std::ceil(-std::log10(step))) + 1, 0, 6);
}

const QVector<double>& valuesFor(const S21PlotTrace& trace, bool phasePanel)
{
    return phasePanel ? trace.phaseDeg : trace.magDb;
}

qsizetype nearestIndex(const QVector<double>& values, double target)
{
    if (values.isEmpty()) {
        return -1;
    }
    const auto it = std::lower_bound(values.cbegin(), values.cend(), target);
    if (it == values.cbegin()) {
        return 0;
    }
    if (it == values.cend()) {
        return values.size() - 1;
    }
    const qsizetype right = std::distance(values.cbegin(), it);
    const qsizetype left = right - 1;
    return std::abs(values[left] - target) <= std::abs(values[right] - target) ? left : right;
}

} // namespace

S21PlotWidget::S21PlotWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(160);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setToolTip(QStringLiteral(
        "Колесо — масштаб; левая кнопка с перетаскиванием — прокрутка; "
        "двойной щелчок — весь диапазон. В режиме маркера щёлкните по графику."));
}

void S21PlotWidget::setCurves(const QVector<double>& freqGhz,
                              const QVector<double>& magDb,
                              const QVector<double>& phaseUnwrapDeg)
{
    S21PlotTrace trace;
    trace.name = m_subtitle.isEmpty() ? QStringLiteral("S21") : m_subtitle;
    trace.freqGhz = freqGhz;
    trace.magDb = magDb;
    trace.phaseDeg = phaseUnwrapDeg;
    setTraces({trace});
}

void S21PlotWidget::setTraces(const QVector<S21PlotTrace>& traces)
{
    m_traces = traces;
    resetView();
}

void S21PlotWidget::resetView()
{
    m_viewLeft = 0.0;
    m_viewRight = 1.0;
    m_dragging = false;
    setCursor(m_markerPlacementEnabled ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

void S21PlotWidget::clearCurves()
{
    m_traces.clear();
    m_overlayFreqGhz.clear();
    m_overlayY.clear();
    m_markerFreqGhz.clear();
    resetView();
}

void S21PlotWidget::setPanelTitles(const QString& magTitle, const QString& phaseTitle)
{
    m_magTitle = magTitle;
    m_phaseTitle = phaseTitle;
    update();
}

void S21PlotWidget::setSinglePanelMode(bool enabled)
{
    m_singlePanel = enabled;
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

void S21PlotWidget::setMarkerFrequencies(const QVector<double>& freqGhz)
{
    m_markerFreqGhz = freqGhz;
    update();
}

void S21PlotWidget::setMarkerPlacementEnabled(bool enabled)
{
    m_markerPlacementEnabled = enabled;
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
}

const S21PlotTrace* S21PlotWidget::primaryTrace() const
{
    for (const auto& trace : m_traces) {
        if (trace.freqGhz.size() >= 2) {
            return &trace;
        }
    }
    return nullptr;
}

qsizetype S21PlotWidget::primaryPointCount() const noexcept
{
    const auto* trace = primaryTrace();
    return trace == nullptr ? 0 : trace->freqGhz.size();
}

void S21PlotWidget::paintPanel(QPainter& p,
                               const QRect& area,
                               const QString& title,
                               bool phasePanel) const
{
    const QPalette pal = palette();
    p.fillRect(area, pal.color(QPalette::Base));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    p.drawRect(area.adjusted(0, 0, -1, -1));

    const QRect plot = area.adjusted(kPlotLeftPad, kPlotTopPad,
                                     -kPlotRightPad, -kPlotBottomPad);
    QFont titleFont = p.font();
    titleFont.setBold(true);
    p.setFont(titleFont);
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(area.adjusted(10, 5, -10, 0), Qt::AlignLeft | Qt::AlignTop, title);
    titleFont.setBold(false);
    p.setFont(titleFont);

    const S21PlotTrace* primary = primaryTrace();
    if (primary == nullptr || plot.width() < 4 || plot.height() < 4) {
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(plot, Qt::AlignCenter | Qt::TextWordWrap, m_emptyHint);
        return;
    }

    const double fullX0 = primary->freqGhz.first();
    const double fullX1 = primary->freqGhz.last();
    const double fullDx = (fullX1 > fullX0) ? (fullX1 - fullX0) : 1.0;
    const double x0 = fullX0 + m_viewLeft * fullDx;
    const double x1 = fullX0 + m_viewRight * fullDx;
    const double dx = (x1 > x0) ? (x1 - x0) : 1.0;

    double yMin = 0.0;
    double yMax = 0.0;
    bool haveFinite = false;
    auto expandValue = [&](double value) {
        if (!std::isfinite(value)) {
            return;
        }
        if (!haveFinite) {
            yMin = value;
            yMax = value;
            haveFinite = true;
        } else {
            yMin = std::min(yMin, value);
            yMax = std::max(yMax, value);
        }
    };

    for (const auto& trace : m_traces) {
        const auto& y = valuesFor(trace, phasePanel);
        if (trace.freqGhz.size() != y.size()) {
            continue;
        }
        const auto first = std::lower_bound(trace.freqGhz.cbegin(), trace.freqGhz.cend(), x0);
        const auto last = std::upper_bound(trace.freqGhz.cbegin(), trace.freqGhz.cend(), x1);
        for (auto it = first; it != last; ++it) {
            expandValue(y[std::distance(trace.freqGhz.cbegin(), it)]);
        }
    }

    const bool drawOverlay = phasePanel && m_overlayY.size() >= 2
        && m_overlayFreqGhz.size() == m_overlayY.size();
    if (drawOverlay) {
        for (double value : m_overlayY) {
            expandValue(value);
        }
    }
    if (!haveFinite || yMax <= yMin) {
        yMin = haveFinite ? yMin - 0.5 : 0.0;
        yMax = haveFinite ? yMax + 0.5 : 1.0;
    }
    const double yStep = niceStep((yMax - yMin) / 6.0);
    yMin = std::floor(yMin / yStep) * yStep;
    yMax = std::ceil(yMax / yStep) * yStep;
    if (yMax <= yMin) {
        yMax = yMin + yStep;
    }
    const double dy = yMax - yMin;

    QColor majorGrid = pal.color(QPalette::Mid);
    majorGrid.setAlpha(115);
    QColor minorGrid = pal.color(QPalette::Mid);
    minorGrid.setAlpha(45);
    const QColor axisText = pal.color(QPalette::Text);
    const QFontMetrics fm(p.font());

    for (int i = 0; i <= kXMajorDivs * kMinorDivs; ++i) {
        const double t = static_cast<double>(i) / (kXMajorDivs * kMinorDivs);
        const int x = plot.left() + static_cast<int>(t * plot.width());
        const bool major = i % kMinorDivs == 0;
        p.setPen(QPen(major ? majorGrid : minorGrid, 1,
                      major ? Qt::SolidLine : Qt::DotLine));
        p.drawLine(x, plot.top(), x, plot.bottom());
        if (!major) {
            continue;
        }
        const int frequencyDecimals = dx < 0.001 ? 6 : (dx < 0.1 ? 5 : 3);
        const QString xLabel = QString::number(x0 + t * dx, 'f', frequencyDecimals);
        const int tw = fm.horizontalAdvance(xLabel);
        const int labelLeft = i == 0 ? plot.left()
            : (i == kXMajorDivs * kMinorDivs ? plot.right() - tw : x - tw / 2);
        p.setPen(axisText);
        p.drawText(QRect(labelLeft, plot.bottom() + 5, tw + 2, 16),
                   Qt::AlignLeft | Qt::AlignTop, xLabel);
    }

    const int yMajorCount = std::max(1, static_cast<int>(std::llround(dy / yStep)));
    for (int i = 0; i <= yMajorCount * kMinorDivs; ++i) {
        const double t = static_cast<double>(i) / (yMajorCount * kMinorDivs);
        const int yPix = plot.bottom() - static_cast<int>(t * plot.height());
        const bool major = i % kMinorDivs == 0;
        p.setPen(QPen(major ? majorGrid : minorGrid, 1,
                      major ? Qt::SolidLine : Qt::DotLine));
        p.drawLine(plot.left(), yPix, plot.right(), yPix);
        if (!major) {
            continue;
        }
        p.setPen(axisText);
        p.drawText(QRect(area.left() + 4, yPix - 8, kPlotLeftPad - 10, 16),
                   Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(yMin + t * dy, 'f', decimalsForStep(yStep)));
    }

    p.setPen(axisText);
    p.drawText(QRect(plot.right() - 115, plot.bottom() + 20, 115, 14),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Частота, ГГц"));
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(axisText, 1));
    p.drawRect(plot.adjusted(0, 0, -1, -1));
    if (yMin < 0.0 && yMax > 0.0) {
        QColor zeroColor = axisText;
        zeroColor.setAlpha(150);
        const int zeroY = plot.bottom() - static_cast<int>((-yMin / dy) * plot.height());
        p.setPen(QPen(zeroColor, 1.2));
        p.drawLine(plot.left(), zeroY, plot.right(), zeroY);
    }

    auto mapPolyline = [&](const QVector<double>& fx, const QVector<double>& fy) {
        QPolygonF poly;
        if (fx.size() != fy.size()) {
            return poly;
        }
        const auto first = std::lower_bound(fx.cbegin(), fx.cend(), x0);
        const auto last = std::upper_bound(fx.cbegin(), fx.cend(), x1);
        poly.reserve(std::distance(first, last));
        for (auto it = first; it != last; ++it) {
            const qsizetype index = std::distance(fx.cbegin(), it);
            if (!std::isfinite(fy[index])) {
                continue;
            }
            poly << QPointF(plot.left() + ((*it - x0) / dx) * plot.width(),
                            plot.bottom() - ((fy[index] - yMin) / dy) * plot.height());
        }
        return poly;
    };

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
    int legendX = plot.right();
    for (auto it = m_traces.crbegin(); it != m_traces.crend(); ++it) {
        const auto& y = valuesFor(*it, phasePanel);
        const QColor color = it->color.isValid() ? it->color : pal.color(QPalette::Highlight);
        p.setPen(QPen(color, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(mapPolyline(it->freqGhz, y));

        if (m_traces.size() > 1 && !it->name.isEmpty()) {
            const int legendWidth = fm.horizontalAdvance(it->name) + 18;
            legendX -= legendWidth;
            p.drawLine(legendX, area.top() + 9, legendX + 10, area.top() + 9);
            p.setPen(pal.color(QPalette::WindowText));
            p.drawText(legendX + 13, area.top() + 13, it->name);
        }
    }
    if (drawOverlay) {
        p.setPen(QPen(QColor(0xC0, 0x55, 0x20), 1.5));
        p.drawPolyline(mapPolyline(m_overlayFreqGhz, m_overlayY));
    }

    for (qsizetype marker = 0; marker < m_markerFreqGhz.size(); ++marker) {
        const double markerFreq = m_markerFreqGhz[marker];
        if (markerFreq < x0 || markerFreq > x1) {
            continue;
        }
        const int markerX = plot.left()
            + static_cast<int>(((markerFreq - x0) / dx) * plot.width());
        p.setPen(QPen(QColor(0xB0, 0x20, 0x20), 1, Qt::DashLine));
        p.drawLine(markerX, plot.top(), markerX, plot.bottom());
        p.drawText(markerX + 3, plot.top() + 12, QStringLiteral("M%1").arg(marker + 1));

        for (const auto& trace : m_traces) {
            const auto& y = valuesFor(trace, phasePanel);
            const qsizetype index = nearestIndex(trace.freqGhz, markerFreq);
            if (index < 0 || index >= y.size() || !std::isfinite(y[index])) {
                continue;
            }
            const double yPixel = plot.bottom() - ((y[index] - yMin) / dy) * plot.height();
            const QColor color = trace.color.isValid() ? trace.color : pal.color(QPalette::Highlight);
            p.setPen(QPen(color, 2));
            p.setBrush(color);
            p.drawEllipse(QPointF(markerX, yPixel), 3.0, 3.0);
        }
    }
    p.setBrush(Qt::NoBrush);
    p.setRenderHint(QPainter::Antialiasing, false);
}

void S21PlotWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Window));
    p.setBrush(Qt::NoBrush);
    QRect r = rect().adjusted(1, 1, -1, -1);
    if (!m_subtitle.isEmpty()) {
        p.setPen(palette().color(QPalette::Mid));
        p.drawText(r.adjusted(6, 0, -6, 0), Qt::AlignLeft | Qt::AlignTop, m_subtitle);
        r.setTop(r.top() + 16);
    }
    if (m_singlePanel) {
        paintPanel(p, r, m_magTitle, false);
    } else {
        const int mid = r.top() + r.height() / 2;
        paintPanel(p, QRect(r.left(), r.top(), r.width(), mid - r.top() - 2), m_magTitle, false);
        paintPanel(p, QRect(r.left(), mid + 2, r.width(), r.bottom() - mid - 2),
                   m_phaseTitle, true);
    }
}

void S21PlotWidget::wheelEvent(QWheelEvent* event)
{
    const S21PlotTrace* primary = primaryTrace();
    if (primary == nullptr || event->angleDelta().y() == 0) {
        event->ignore();
        return;
    }
    const double plotWidth = std::max(1, width() - (kPlotLeftPad + kPlotRightPad + 1));
    const double cursor = std::clamp((event->position().x() - kPlotLeftPad) / plotWidth, 0.0, 1.0);
    const double oldSpan = m_viewRight - m_viewLeft;
    const double factor = std::pow(0.8, event->angleDelta().y() / 120.0);
    const double minSpan = std::max(1.0 / static_cast<double>(primary->freqGhz.size() - 1), 0.002);
    const double newSpan = std::clamp(oldSpan * factor, minSpan, 1.0);
    const double anchor = m_viewLeft + cursor * oldSpan;
    m_viewLeft = std::clamp(anchor - cursor * newSpan, 0.0, 1.0 - newSpan);
    m_viewRight = m_viewLeft + newSpan;
    update();
    event->accept();
}

void S21PlotWidget::mousePressEvent(QMouseEvent* event)
{
    const S21PlotTrace* primary = primaryTrace();
    if (event->button() == Qt::LeftButton && m_markerPlacementEnabled && primary != nullptr) {
        const double plotWidth = std::max(1, width() - (kPlotLeftPad + kPlotRightPad + 1));
        const double cursor = std::clamp((event->position().x() - kPlotLeftPad) / plotWidth, 0.0, 1.0);
        const double fullX0 = primary->freqGhz.first();
        const double fullDx = std::max(primary->freqGhz.last() - fullX0, 0.0);
        const double fraction = m_viewLeft + cursor * (m_viewRight - m_viewLeft);
        emit markerRequested(fullX0 + fraction * fullDx);
        event->accept();
        return;
    }
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
        setCursor(m_markerPlacementEnabled ? Qt::CrossCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void S21PlotWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && !m_markerPlacementEnabled) {
        resetView();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
