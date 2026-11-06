#include "ChannelRun.h"

#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

QFont reportFont()
{
    static const int id = [] {
        QFile file(QStringLiteral(":/heter/fonts/DejaVuSans.ttf"));
        if (!file.open(QIODevice::ReadOnly)) {
            return -1;
        }
        return QFontDatabase::addApplicationFontFromData(file.readAll());
    }();
    if (id >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) {
            return QFont(families.front());
        }
    }
    return QFont(QStringLiteral("Arial"));
}

int bounded(int value, int maxCode)
{
    return std::clamp(value, 0, std::max(0, maxCode));
}

}  // namespace

QVector<ChannelStep> buildChannelSteps(const QVector<int>& channels, int phaseMax, int attenMax,
                                       const std::array<int, 4>& holdPhase, const std::array<int, 4>& holdAtten)
{
    QVector<ChannelStep> steps;
    const int phaseLast = std::max(0, phaseMax);
    const int attenLast = std::max(0, attenMax);
    const int bothLast = std::min(phaseLast, attenLast);
    for (const int channel : channels) {
        if (channel < 0 || channel > 3) {
            continue;
        }
        const int holdPh = bounded(holdPhase[static_cast<std::size_t>(channel)], phaseLast);
        const int holdAt = bounded(holdAtten[static_cast<std::size_t>(channel)], attenLast);
        for (int code = 0; code <= attenLast; ++code) {
            steps.append(ChannelStep{channel, 0, holdPh, code});
        }
        for (int code = 0; code <= phaseLast; ++code) {
            steps.append(ChannelStep{channel, 1, code, holdAt});
        }
        for (int code = 0; code <= bothLast; ++code) {
            steps.append(ChannelStep{channel, 2, code, code});
        }
    }
    return steps;
}

QString channelStageName(int stage)
{
    switch (stage) {
    case 0:
        return QStringLiteral("Аттенюация");
    case 1:
        return QStringLiteral("Фаза");
    default:
        return QStringLiteral("Фаза и аттенюация");
    }
}

double channelMidDb(const QVector<double>& magDb)
{
    if (magDb.isEmpty()) {
        return -120.0;
    }
    const double value = magDb.at(magDb.size() / 2);
    return std::isfinite(value) ? value : -120.0;
}

QImage renderChannelGraph(const ChannelPoint& point, const QSize& size)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QFont font = reportFont();
    font.setPixelSize(14);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#193448")));
    const QString title = QStringLiteral("Канал %1 · %2 · фаза %3° · аттенюация %4 дБ")
                              .arg(point.step.channel + 1)
                              .arg(channelStageName(point.step.stage))
                              .arg(point.phaseDeg, 0, 'f', 3)
                              .arg(point.attenDb, 0, 'f', 2);
    painter.drawText(QRect(12, 8, size.width() - 24, 24), title);

    const QRect plot(58, 40, size.width() - 78, size.height() - 68);
    painter.setPen(QColor(QStringLiteral("#c7d5d9")));
    painter.drawRect(plot);

    const QVector<double>* traces[4] = {&point.s11mag, &point.s21mag, &point.s12mag, &point.s22mag};
    const QColor colors[4] = {QColor(QStringLiteral("#1d4ed8")), QColor(QStringLiteral("#c2410c")),
                              QColor(QStringLiteral("#047857")), QColor(QStringLiteral("#7c3aed"))};
    const QString names[4] = {QStringLiteral("S11"), QStringLiteral("S21"), QStringLiteral("S12"),
                              QStringLiteral("S22")};
    double yMin = -80.0;
    double yMax = 0.0;
    int count = 0;
    for (const QVector<double>* trace : traces) {
        count = std::max(count, static_cast<int>(trace->size()));
        for (const double value : *trace) {
            if (!std::isfinite(value)) {
                continue;
            }
            yMin = std::min(yMin, value);
            yMax = std::max(yMax, value);
        }
    }
    if (!(yMax > yMin)) {
        yMax = yMin + 1.0;
    }
    const double span = yMax - yMin;
    yMin -= span * 0.05;
    yMax += span * 0.05;

    const auto xOf = [&](int index) {
        if (count <= 1) {
            return plot.left();
        }
        return plot.left() + (plot.width() - 1) * index / (count - 1);
    };
    const auto yOf = [&](double db) {
        const double t = (db - yMin) / (yMax - yMin);
        return plot.bottom() - t * (plot.height() - 1);
    };

    font.setPixelSize(11);
    painter.setFont(font);
    painter.setPen(QColor(QStringLiteral("#193448")));
    painter.drawText(plot.left(), plot.bottom() + 16, QStringLiteral("частота"));
    painter.drawText(8, plot.top() + 12, QStringLiteral("%1 дБ").arg(yMax, 0, 'f', 0));
    painter.drawText(8, plot.bottom(), QStringLiteral("%1").arg(yMin, 0, 'f', 0));

    for (int t = 0; t < 4; ++t) {
        const QVector<double>& mag = *traces[t];
        if (mag.size() < 2) {
            continue;
        }
        QPen pen(colors[t], 1.6);
        painter.setPen(pen);
        QPointF previous;
        bool started = false;
        const int n = std::min(count, static_cast<int>(mag.size()));
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(mag.at(i))) {
                started = false;
                continue;
            }
            const QPointF current(xOf(i), yOf(mag.at(i)));
            if (started) {
                painter.drawLine(previous, current);
            }
            previous = current;
            started = true;
        }
    }
    int legendX = plot.right() - 210;
    for (int t = 0; t < 4; ++t) {
        painter.setPen(colors[t]);
        painter.drawLine(legendX, 22, legendX + 16, 22);
        painter.setPen(QColor(QStringLiteral("#193448")));
        painter.drawText(legendX + 20, 26, names[t]);
        legendX += 52;
    }
    return image;
}

QString writeChannelReport(const QString& folder, const QString& title, const QVector<ChannelPoint>& points)
{
    const QString path = folder + QStringLiteral("/report.pdf");
    QPdfWriter pdf(path);
    pdf.setTitle(title);
    pdf.setPageSize(QPageSize(QPageSize::A4));
    pdf.setPageOrientation(QPageLayout::Landscape);
    pdf.setResolution(96);
    QPainter painter(&pdf);
    if (!painter.isActive()) {
        return QStringLiteral("не удалось открыть PDF");
    }
    QFont font = reportFont();
    const qreal w = painter.viewport().width();
    const qreal h = painter.viewport().height();
    int page = 1;
    const auto label = [&](qreal x, qreal y, const QString& text, int size) {
        font.setPixelSize(size);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#193448")));
        painter.drawText(QPointF(x, y), text);
    };
    const auto footer = [&] {
        label(28, h - 18, title, 9);
        label(w - 90, h - 18, QStringLiteral("стр. %1").arg(page), 9);
    };

    label(28, 36, QStringLiteral("Протокол измерения каналов ППКХ4"), 18);
    label(28, 58, title, 11);
    label(28, 78, QStringLiteral("Точек: %1. В таблице |S| на средней частоте свипа, дБ.").arg(points.size()), 11);

    const QStringList headers{QStringLiteral("Канал"), QStringLiteral("Этап"), QStringLiteral("Фаза"),
                              QStringLiteral("Аттен."), QStringLiteral("S11"), QStringLiteral("S21"),
                              QStringLiteral("S12"), QStringLiteral("S22")};
    const qreal colW = (w - 56) / headers.size();
    qreal y = 100;
    const auto headerRow = [&] {
        font.setPixelSize(10);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#13725e")));
        for (int c = 0; c < headers.size(); ++c) {
            painter.drawText(QRectF(28 + c * colW, y, colW - 4, 16), headers.at(c));
        }
        y += 18;
    };
    headerRow();
    for (const ChannelPoint& point : points) {
        if (y > h - 36) {
            footer();
            pdf.newPage();
            ++page;
            y = 36;
            headerRow();
        }
        const QStringList cells{QString::number(point.step.channel + 1), channelStageName(point.step.stage),
                                QStringLiteral("%1°").arg(point.phaseDeg, 0, 'f', 1),
                                QStringLiteral("%1 дБ").arg(point.attenDb, 0, 'f', 1),
                                QString::number(point.s11Db, 'f', 1), QString::number(point.s21Db, 'f', 1),
                                QString::number(point.s12Db, 'f', 1), QString::number(point.s22Db, 'f', 1)};
        font.setPixelSize(9);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#193448")));
        for (int c = 0; c < cells.size(); ++c) {
            painter.drawText(QRectF(28 + c * colW, y, colW - 4, 14), cells.at(c));
        }
        y += 15;
    }
    footer();

    for (const ChannelPoint& point : points) {
        if (point.imageFile.isEmpty() || !QFile::exists(point.imageFile)) {
            continue;
        }
        const QImage image(point.imageFile);
        if (image.isNull()) {
            continue;
        }
        pdf.newPage();
        ++page;
        label(28, 28, QStringLiteral("Канал %1 · %2 · код фазы %3 · код аттенюации %4")
                          .arg(point.step.channel + 1)
                          .arg(channelStageName(point.step.stage))
                          .arg(point.step.phase)
                          .arg(point.step.atten),
              12);
        const QRect target(28, 40, static_cast<int>(w - 56), static_cast<int>(h - 70));
        painter.drawImage(target, image);
        footer();
    }
    painter.end();
    return QString();
}
