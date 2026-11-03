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
    pdf.setResolution(72);
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
    const auto header = [&](const QString& title, const QString& subtitle) {
        label(32, h - 31, QStringLiteral("СТЕНД ВЧ    /    ADF4355-3"), 10, QColor(QStringLiteral("#13725e")));
        label(32, h - 63, title, 18, QColor(QStringLiteral("#193448")));
        label(32, h - 83, subtitle, 10, QColor(QStringLiteral("#193448")));
        painter.setPen(QColor(QStringLiteral("#c7d5d9")));
        painter.drawLine(QPointF(32, h - 94), QPointF(w - 32, h - 94));
    };
    const auto footer = [&] {
        label(32, 17, run.id + QStringLiteral("   •   ") + run.started, 8, QColor(QStringLiteral("#193448")));
        label(w - 84, 17, QStringLiteral("стр. %1").arg(page), 8, QColor(QStringLiteral("#193448")));
    };
    const auto nextPage = [&] {
        footer();
        pdf.newPage();
        ++page;
    };

    QHash<QString, double> baselines;
    for (const HeterReportPoint& point : points) {
        if (point.signal && point.lock && std::abs(point.stepDb - run.attMin) < 0.01) {
            baselines.insert(point.port + QLatin1Char('|') + point.mode + QLatin1Char('|') + QString::number(point.setHz, 'f', 0),
                             point.levelDbm);
        }
    }

    header(QStringLiteral("Протокол измерений гетеродина"),
           QStringLiteral("%1   •   точек %2 из %3").arg(run.outcome).arg(points.size()).arg(run.total));
    QStringList lines;
    lines << QStringLiteral("Анализатор    %1    /    %2")
                 .arg(run.instrument.isEmpty() ? QStringLiteral("нет идентификации") : run.instrument, run.analyzerIp);
    lines << QStringLiteral("Питание    ручная установка    /    IP блока %1")
                 .arg(run.psuIp.isEmpty() ? QStringLiteral("не указан") : run.psuIp);
    lines << QStringLiteral("Серия    %1 — %2    /    RS-485 %3")
                 .arg(run.started, run.ended, run.rs485Port.isEmpty() ? QStringLiteral("—") : run.rs485Port);
    lines << QStringLiteral("Тракт    %1    /    ослабление %2…%3 дБ, шаг 0,5 дБ")
                 .arg(run.routing)
                 .arg(run.attMax, 0, 'g', 4)
                 .arg(run.attMin, 0, 'g', 4);
    lines << QStringLiteral("Частоты задания, МГц    ") + run.frequencies;
    if (run.haveInitial) {
        lines << QStringLiteral("Опора    %1 50 МГц    /    мощность A, B по настройке: %2, %3 дБм")
                     .arg(run.externalRef ? QStringLiteral("внешняя") : QStringLiteral("на плате"))
                     .arg(run.aDbm)
                     .arg(run.bDbm);
    } else {
        lines << QStringLiteral("Исходные параметры платы не получены.");
    }
    lines << QStringLiteral("Поиск пика ±%1 МГц. При превышении медианы трассы на 10 дБ — уточнение в полосе обзора 2 МГц.")
                 .arg(run.spanHz / 2e6, 0, 'g', 4);
    lines << QStringLiteral("Ослабление по уровню = уровень при минимальной уставке серии минус уровень в точке. "
                            "Для слабого пика или LOCK=0 — прочерк.");
    lines << QStringLiteral("Уровень указан на входе анализатора. Поправки на внешние кабели и делители не применялись.");
    if (!run.error.isEmpty()) {
        lines << run.error;
    }
    qreal y = h - 115;
    for (const QString& line : lines) {
        for (const QString& part : wrap(line, 110)) {
            label(32, y, part, 9, QColor(QStringLiteral("#193448")));
            y -= 14;
        }
    }
    y -= 8;
    const qreal cols[] = {34, 68, 120, 183, 271, 358, 465, 584, 650, 726};
    const QStringList titles{QStringLiteral("№"), QStringLiteral("Тракт"), QStringLiteral("Вкл."),
                             QStringLiteral("ATT A, дБ"), QStringLiteral("ATT B, дБ"), QStringLiteral("Зад., МГц"),
                             QStringLiteral("Пик, МГц"), QStringLiteral("дБм"), QStringLiteral("Ослаб., дБ"),
                             QStringLiteral("Статус")};
    const auto tableHeader = [&](qreal top) {
        painter.fillRect(QRectF(32, top - 14, w - 64, 17), QColor(QStringLiteral("#deeee8")));
        for (int i = 0; i < titles.size(); ++i) {
            label(cols[i], top, titles.at(i), 8, QColor(QStringLiteral("#193448")));
        }
        return top - 19;
    };
    y = tableHeader(y);
    for (const HeterReportPoint& point : points) {
        if (y < 40) {
            nextPage();
            header(QStringLiteral("Таблица измерений"),
                   QStringLiteral("Номер строки совпадает с номером снимка в приложении."));
            y = tableHeader(h - 120);
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
                                 QString::number(point.setHz / 1e6, 'f', 6),
                                 QString::number(point.peakHz / 1e6, 'f', 6),
                                 QString::number(point.levelDbm, 'f', 2),
                                 loss,
                                 status};
        for (int i = 0; i < values.size(); ++i) {
            label(cols[i], y, values.at(i), 8, QColor(QStringLiteral("#193448")));
        }
        y -= 15;
    }
    if (points.isEmpty()) {
        footer();
        painter.end();
        return {};
    }
    nextPage();
    for (int index = 0; index < points.size(); ++index) {
        const HeterReportPoint& point = points.at(index);
        const QString status = !point.signal ? QStringLiteral("слабый пик")
                               : point.lock  ? QStringLiteral("захват")
                                             : QStringLiteral("нет LOCK");
        header(QStringLiteral("Снимок %1   /   выход %2   /   включены %3")
                   .arg(point.n, 4, 10, QLatin1Char('0'))
                   .arg(point.port, point.mode),
               QStringLiteral("Задано %1 МГц     Пик %2 МГц     Уровень %3 дБм     LOCK %4")
                   .arg(point.setHz / 1e6, 0, 'f', 6)
                   .arg(point.peakHz / 1e6, 0, 'f', 6)
                   .arg(point.levelDbm, 0, 'f', 2)
                   .arg(point.lock ? 1 : 0));
        QImage image(folder + QLatin1Char('/') + point.imageFile);
        if (!image.isNull()) {
            const QRectF box(32, 63, w - 64, h - 171);
            const QSizeF fitted = image.size().scaled(box.size().toSize(), Qt::KeepAspectRatio);
            const QRectF target(box.center().x() - fitted.width() / 2.0, box.center().y() - fitted.height() / 2.0,
                                fitted.width(), fitted.height());
            painter.drawImage(target, image);
        }
        label(32, 45,
              QStringLiteral("ATT A/B %1 / %2 дБ    |    Обзор %3 МГц    |    RBW %4 кГц    |    %5    |    %6")
                  .arg(point.attA, 0, 'f', 1)
                  .arg(point.attB, 0, 'f', 1)
                  .arg(point.spanHz / 1e6, 0, 'g', 4)
                  .arg(point.rbwHz / 1000.0, 0, 'g', 4)
                  .arg(status, point.time),
              9, QColor(QStringLiteral("#193448")));
        if (index + 1 < points.size()) {
            nextPage();
        } else {
            footer();
        }
    }
    painter.end();
    return {};
}
