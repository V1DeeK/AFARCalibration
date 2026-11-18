#include "HeterReport.h"

#include <QFile>
#include <QFont>
#include <QHash>
#include <QFontDatabase>
#include <QImage>
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

QStringList wrap(const QString& text, int width)
{
    QStringList lines;
    QString rest = text;
    while (rest.size() > width) {
        int cut = rest.lastIndexOf(QLatin1Char(' '), width);
        if (cut < 20) {
            cut = width;
        }
        lines << rest.left(cut);
        rest = rest.mid(cut).trimmed();
    }
    if (!rest.isEmpty()) {
        lines << rest;
    }
    return lines;
}

}  // namespace

QString writeHeterodyneReport(const QString& folder, const HeterReportRun& run, const QVector<HeterReportPoint>& points)
{
    const QString path = folder + QStringLiteral("/report.pdf");
    QPdfWriter pdf(path);
    pdf.setTitle(QStringLiteral("Протокол измерений гетеродина • ") + run.id);
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

    const auto label = [&](qreal x, qreal y, const QString& text, int size, const QColor& color) {
        font.setPixelSize(size);
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QPointF(x, y), text);
    };
    const auto footer = [&] {
        label(28, h - 18, run.id + QStringLiteral("   •   ") + run.started, 9, QColor(QStringLiteral("#193448")));
        label(w - 90, h - 18, QStringLiteral("стр. %1").arg(page), 9, QColor(QStringLiteral("#193448")));
    };
    QHash<QString, double> baselines;
    for (const HeterReportPoint& point : points) {
        if (point.signal && point.lock && std::abs(point.stepDb - run.attMin) < 0.01) {
            baselines.insert(point.port + QLatin1Char('|') + point.mode + QLatin1Char('|') + QString::number(point.setHz, 'f', 0),
                             point.levelDbm);
        }
    }

    label(28, 36, QStringLiteral("Протокол измерений гетеродина"), 18, QColor(QStringLiteral("#193448")));
    label(28, 58, QStringLiteral("%1   •   точек %2 из %3").arg(run.outcome).arg(points.size()).arg(run.total), 11,
          QColor(QStringLiteral("#193448")));
    QStringList lines;
    lines << QStringLiteral("Анализатор    %1    /    %2")
                 .arg(run.instrument.isEmpty() ? QStringLiteral("нет идентификации") : run.instrument, run.analyzerIp);
    lines << QStringLiteral("Серия    %1 — %2    /    RS-485 %3")
                 .arg(run.started, run.ended, run.rs485Port.isEmpty() ? QStringLiteral("—") : run.rs485Port);
    lines << QStringLiteral("Тракт    %1    /    ослабление %2…%3 дБ, шаг 0,5 дБ")
                 .arg(run.routing)
                 .arg(run.attMax, 0, 'g', 4)
                 .arg(run.attMin, 0, 'g', 4);
    double freqLo = 0;
    double freqHi = 0;
    if (!points.isEmpty()) {
        freqLo = points.first().setHz;
        freqHi = points.first().setHz;
        for (const HeterReportPoint& point : points) {
            freqLo = std::min(freqLo, point.setHz);
            freqHi = std::max(freqHi, point.setHz);
        }
    }
    lines << QStringLiteral("Частоты    %1…%2 МГц. В таблице — все %3 точек, каждая частота и каждый код ослабления.")
                 .arg(freqLo / 1e6, 0, 'f', 0)
                 .arg(freqHi / 1e6, 0, 'f', 0)
                 .arg(points.size());
    if (run.haveInitial) {
        lines << QStringLiteral("Опора    %1 50 МГц    /    мощность A, B по настройке: %2, %3 дБм")
                     .arg(run.externalRef ? QStringLiteral("внешняя") : QStringLiteral("на плате"))
                     .arg(run.aDbm)
                     .arg(run.bDbm);
    } else {
        lines << QStringLiteral("Исходные параметры платы не получены.");
    }
    lines << QStringLiteral("Поиск пика ±%1 МГц, 1000 точек, разрешающая полоса 10 кГц. Уровень — на входе анализатора.")
                 .arg(run.spanHz / 2e6, 0, 'g', 4);
    if (!run.error.isEmpty()) {
        lines << run.error;
    }
    qreal y = 80;
    for (const QString& line : lines) {
        for (const QString& part : wrap(line, 120)) {
            label(28, y, part, 10, QColor(QStringLiteral("#193448")));
            y += 16;
        }
    }
    y += 10;
    const QStringList titles{QStringLiteral("№"), QStringLiteral("Выход"), QStringLiteral("Вкл."),
                             QStringLiteral("ATT A, дБ"), QStringLiteral("ATT B, дБ"), QStringLiteral("Задано, МГц"),
                             QStringLiteral("Пик, МГц"), QStringLiteral("дБм"), QStringLiteral("Ослабл., дБ"),
                             QStringLiteral("Статус")};
    const qreal tableLeft = 24;
    const qreal tableW = w - 48;
    const qreal colW = tableW / titles.size();
    const qreal rowH = 16;
    const auto paintRow = [&](const QStringList& cells, bool headerRow) {
        if (headerRow) {
            painter.fillRect(QRectF(tableLeft, y, tableW, rowH), QColor(QStringLiteral("#deeee8")));
        }
        painter.setPen(QPen(QColor(QStringLiteral("#334155")), 1));
        painter.drawRect(QRectF(tableLeft, y, tableW, rowH));
        for (int c = 1; c < cells.size(); ++c) {
            painter.drawLine(QPointF(tableLeft + c * colW, y), QPointF(tableLeft + c * colW, y + rowH));
        }
        font.setPixelSize(headerRow ? 10 : 9);
        font.setBold(headerRow);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#193448")));
        for (int c = 0; c < cells.size(); ++c) {
            painter.drawText(QRectF(tableLeft + c * colW + 4, y, colW - 8, rowH),
                             Qt::AlignVCenter | Qt::AlignLeft, cells.at(c));
        }
        font.setBold(false);
        y += rowH;
    };
    QVector<HeterReportPoint> tableRows = points;
    std::sort(tableRows.begin(), tableRows.end(), [](const HeterReportPoint& a, const HeterReportPoint& b) {
        if (a.setHz != b.setHz) {
            return a.setHz < b.setHz;
        }
        if (a.stepDb != b.stepDb) {
            return a.stepDb > b.stepDb;
        }
        return a.n < b.n;
    });
    paintRow(titles, true);
    for (const HeterReportPoint& point : tableRows) {
        if (y + rowH > h - 28) {
            footer();
            pdf.newPage();
            ++page;
            y = 36;
            label(28, y, QStringLiteral("Таблица измерений — все частоты и коды ослабления"), 12,
                  QColor(QStringLiteral("#193448")));
            y = 58;
            paintRow(titles, true);
        }
        const QString key = point.port + QLatin1Char('|') + point.mode + QLatin1Char('|')
                            + QString::number(point.setHz, 'f', 0);
        QString loss = QStringLiteral("—");
        if (point.signal && point.lock && baselines.contains(key)) {
            loss = QString::number(baselines.value(key) - point.levelDbm, 'f', 2);
        }
        const QString status = !point.signal ? QStringLiteral("слабый пик")
                               : point.lock  ? QStringLiteral("захват")
                                             : QStringLiteral("нет LOCK");
        const QStringList values{QString::number(point.n),
                                 point.port,
                                 point.mode,
                                 QString::number(point.attA, 'f', 1),
                                 QString::number(point.attB, 'f', 1),
                                 QString::number(point.setHz / 1e6, 'f', 3),
                                 QString::number(point.peakHz / 1e6, 'f', 3),
                                 QString::number(point.levelDbm, 'f', 2),
                                 loss,
                                 status};
        paintRow(values, false);
    }
    footer();
    painter.end();
    return {};
}
