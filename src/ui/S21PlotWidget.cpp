#include "S21PlotWidget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

S21PlotWidget::S21PlotWidget(QWidget* parent)
    : QWidget(parent)
{
    setMinimumHeight(220);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setToolTip(QStringLiteral(
        "Колесо мыши — приблизить/отдалить; зажать левую кнопку — прокрутить; "
        "двойной щелчок — показать весь диапазон"));
}

void S21PlotWidget::setTraceName(const QString& name)
{
    m_traceName = name;
    update();
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
    resetView();
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

    const QRect plot = area.adjusted(54, 18, -8, -28);
    p.setPen(pal.color(QPalette::WindowText));
    p.drawText(area.adjusted(6, 2, -6, 0), Qt::AlignLeft | Qt::AlignTop, title);

    if (y.size() < 2 || m_freqGhz.size() != y.size() || plot.width() < 4 || plot.height() < 4) {
        p.setPen(pal.color(QPalette::Mid));
        p.drawText(plot, Qt::AlignCenter,
                   QStringLiteral("Нет данных свипа"));
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
    for (qsizetype i = firstIndex; i < lastIndex; ++i) {
        const double v = y[i];
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
    const double ySpan = yMax - yMin;
    const double yScale = std::max({1.0, std::abs(yMin), std::abs(yMax)});
    if (!haveFinite || ySpan <= 1.0e-9 * yScale) {
        const double center = std::isfinite(yMin) ? yMin : 0.0;
        yMin = center - 0.5;
        yMax = center + 0.5;
    } else if (yMax <= yMin) {
        yMin = 0.0;
        yMax = 1.0;
    } else {
        const double pad = ySpan * 0.08;
        yMin -= pad;
        yMax += pad;
    }

    constexpr int kGridDivisions = 5;
    p.setPen(QPen(pal.color(QPalette::Midlight), 1, Qt::DashLine));
    for (int i = 0; i <= kGridDivisions; ++i) {
        const int x = plot.left() + (plot.width() * i) / kGridDivisions;
        const int y = plot.top() + (plot.height() * i) / kGridDivisions;
        p.drawLine(x, plot.top(), x, plot.bottom());
        p.drawLine(plot.left(), y, plot.right(), y);
    }

    p.setPen(pal.color(QPalette::Mid));
    for (int i = 0; i <= kGridDivisions; ++i) {
        const double yValue = yMax - (yMax - yMin) * i / kGridDivisions;
        const int y = plot.top() + (plot.height() * i) / kGridDivisions;
        p.drawText(QRect(area.left() + 2, y - 7, 48, 14),
                   Qt::AlignRight | Qt::AlignVCenter, QString::number(yValue, 'f', 1));
    }
    p.drawText(QRect(area.left() + 2, plot.center().y() - 7, 48, 14),
               Qt::AlignLeft | Qt::AlignVCenter, yUnit);
    p.drawText(QRect(plot.left(), plot.bottom() + 2, 90, 16), Qt::AlignLeft,
               QString::number(x0, 'f', 6));
    p.drawText(QRect(plot.center().x() - 45, plot.bottom() + 2, 90, 16), Qt::AlignCenter,
               QString::number((x0 + x1) / 2.0, 'f', 6));
    p.drawText(QRect(plot.right() - 90, plot.bottom() + 2, 90, 16), Qt::AlignRight,
               QString::number(x1, 'f', 6));
    p.drawText(QRect(plot.center().x() - 70, plot.bottom() + 14, 140, 14), Qt::AlignCenter,
               QStringLiteral("Частота, ГГц"));

    QPolygonF poly;
    poly.reserve(lastIndex - firstIndex);
    for (qsizetype i = firstIndex; i < lastIndex; ++i) {
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
    paintPanel(p, top, QStringLiteral("|%1|").arg(m_traceName), QStringLiteral("дБ"), m_magDb);
    paintPanel(p, bottom, QStringLiteral("Фаза %1 (unwrap)").arg(m_traceName),
               QStringLiteral("°"), m_phaseDeg);
}

void S21PlotWidget::wheelEvent(QWheelEvent* event)
{
    if (m_freqGhz.size() < 2 || event->angleDelta().y() == 0) {
        event->ignore();
        return;
    }
    const double width = std::max(1, this->width() - 63);
    const double cursor = std::clamp((event->position().x() - 55.0) / width, 0.0, 1.0);
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
        / std::max(1, width() - 63) * span;
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
