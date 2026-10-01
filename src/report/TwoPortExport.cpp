#include "TwoPortExport.h"

#include <QFile>
#include <QDateTime>
#include <QFont>
#include <QFontMetricsF>
#include <QLocale>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

namespace afar::report {
namespace {

QString pathToQString(const std::filesystem::path& path)
{
#ifdef _WIN32
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

double magnitudeDb(const std::complex<double>& value)
{
    const double magnitude = std::abs(value);
    return magnitude > 0.0 ? 20.0 * std::log10(magnitude)
                           : -std::numeric_limits<double>::infinity();
}

const std::vector<std::complex<double>>& traceFor(const ComplexSweep& sweep, int index)
{
    switch (index) {
    case 0: return sweep.s11;
    case 1: return sweep.s21;
    case 2: return sweep.s12;
    default: return sweep.s22;
    }
}

struct TraceSummary {
    bool valid{false};
    double minimum_db{0.0};
    double maximum_db{0.0};
    double average_db{0.0};
    std::size_t minimum_index{0};
    std::size_t maximum_index{0};
    std::size_t average_index{0};
};

TraceSummary summarize(const std::vector<std::complex<double>>& trace)
{
    TraceSummary result;
    long double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < trace.size(); ++index) {
        const double db = magnitudeDb(trace[index]);
        if (!std::isfinite(db)) {
            continue;
        }
        if (!result.valid) {
            result.minimum_db = db;
            result.maximum_db = db;
            result.minimum_index = index;
            result.maximum_index = index;
            result.valid = true;
        } else {
            if (db < result.minimum_db) {
                result.minimum_db = db;
                result.minimum_index = index;
            }
            if (db > result.maximum_db) {
                result.maximum_db = db;
                result.maximum_index = index;
            }
        }
        sum += db;
        ++count;
    }
    if (count > 0) {
        result.average_db = static_cast<double>(sum / count);
        double nearest = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < trace.size(); ++index) {
            const double db = magnitudeDb(trace[index]);
            const double distance = std::abs(db - result.average_db);
            if (std::isfinite(distance) && distance < nearest) {
                nearest = distance;
                result.average_index = index;
            }
        }
    }
    return result;
}

std::vector<double> unwrappedPhaseDeg(const std::vector<std::complex<double>>& trace)
{
    constexpr double radiansToDegrees = 180.0 / 3.14159265358979323846;
    std::vector<double> result;
    result.reserve(trace.size());
    double offset = 0.0;
    double previous = 0.0;
    for (std::size_t index = 0; index < trace.size(); ++index) {
        double phase = std::arg(trace[index]) * radiansToDegrees;
        if (index > 0) {
            const double delta = phase + offset - previous;
            if (delta > 180.0) {
                offset -= 360.0;
            } else if (delta < -180.0) {
                offset += 360.0;
            }
        }
        phase += offset;
        result.push_back(phase);
        previous = phase;
    }
    return result;
}

std::size_t nearestFrequencyIndex(const std::vector<std::uint64_t>& frequency,
                                  std::uint64_t target)
{
    const auto right = std::lower_bound(frequency.begin(), frequency.end(), target);
    if (right == frequency.begin()) {
        return 0;
    }
    if (right == frequency.end()) {
        return frequency.size() - 1;
    }
    const auto rightIndex = static_cast<std::size_t>(std::distance(frequency.begin(), right));
    const auto leftIndex = rightIndex - 1;
    return target - frequency[leftIndex] <= frequency[rightIndex] - target
        ? leftIndex : rightIndex;
}

QString humanDateTime(const std::string& isoUtc)
{
    const QString source = QString::fromStdString(isoUtc);
    QDateTime date = QDateTime::fromString(source, Qt::ISODateWithMs);
    if (!date.isValid()) {
        date = QDateTime::fromString(source, Qt::ISODate);
    }
    if (!date.isValid()) {
        return source.isEmpty() ? QStringLiteral("не указана") : source;
    }
    date = date.toLocalTime();
    const QLocale ru(QLocale::Russian, QLocale::Russia);
    return ru.toString(date, QStringLiteral("dd MMMM yyyy 'г.,' HH:mm:ss"))
        + QStringLiteral(" (%1)").arg(date.timeZoneAbbreviation());
}

struct InstrumentInfo {
    QString vendor;
    QString model;
    QString serial;
    QString firmware;
};

InstrumentInfo parseInstrument(const std::string& idn)
{
    const QStringList fields = QString::fromStdString(idn).split(QLatin1Char(','));
    return {
        fields.value(0).trimmed(), fields.value(1).trimmed(),
        fields.value(2).trimmed(), fields.value(3).trimmed()
    };
}

QString traceDescription(int index)
{
    constexpr std::array<const char*, 4> descriptions{
        "Коэффициент отражения входного порта 1",
        "Прямой коэффициент передачи: порт 1 -> порт 2",
        "Обратный коэффициент передачи: порт 2 -> порт 1",
        "Коэффициент отражения выходного порта 2"
    };
    return QString::fromUtf8(descriptions[static_cast<std::size_t>(index)]);
}

void drawFooter(QPainter& painter, const QRectF& page, int pageNumber, int pageCount)
{
    painter.save();
    painter.setPen(QColor(105, 115, 130));
    painter.drawLine(QPointF(page.left() + 24, page.bottom() - 25),
                     QPointF(page.right() - 24, page.bottom() - 25));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 7));
    painter.drawText(QRectF(page.left() + 24, page.bottom() - 21,
                            page.width() - 48, 16),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("AFAR RX Calibration Studio | Протокол S-параметров"));
    painter.drawText(QRectF(page.left() + 24, page.bottom() - 21,
                            page.width() - 48, 16),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("Страница %1 из %2").arg(pageNumber).arg(pageCount));
    painter.restore();
}

void drawCard(QPainter& painter, const QRectF& area, const QString& title,
              const QStringList& lines)
{
    painter.save();
    painter.setPen(QPen(QColor(211, 219, 230), 1.0));
    painter.setBrush(QColor(247, 249, 252));
    painter.drawRoundedRect(area, 7, 7);
    painter.setPen(QColor(28, 57, 91));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 10, QFont::Bold));
    painter.drawText(area.adjusted(14, 10, -14, -10), Qt::AlignLeft | Qt::AlignTop, title);
    painter.setPen(QColor(40, 48, 60));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
    qreal y = area.top() + 32;
    for (const QString& line : lines) {
        painter.drawText(QRectF(area.left() + 14, y, area.width() - 28, 18),
                         Qt::AlignLeft | Qt::AlignVCenter, line);
        y += 19;
    }
    painter.restore();
}

void drawMetricCard(QPainter& painter, const QRectF& area, const QString& label,
                    double valueDb, std::uint64_t frequencyHz, const QColor& accent)
{
    painter.save();
    painter.setPen(QPen(QColor(218, 224, 233), 1.0));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(area, 6, 6);
    painter.fillRect(QRectF(area.left(), area.top(), 5, area.height()), accent);
    painter.setPen(accent.darker(120));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 7, QFont::Bold));
    painter.drawText(area.adjusted(15, 7, -8, -7), Qt::AlignLeft | Qt::AlignTop, label);
    painter.setPen(QColor(24, 34, 48));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 13, QFont::Bold));
    painter.drawText(area.adjusted(15, 23, -8, -7), Qt::AlignLeft | Qt::AlignTop,
                     QStringLiteral("%1 дБ").arg(valueDb, 0, 'f', 3));
    painter.setPen(QColor(92, 103, 117));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 7));
    painter.drawText(area.adjusted(15, 49, -8, -5), Qt::AlignLeft | Qt::AlignTop,
                     QStringLiteral("%1 ГГц").arg(static_cast<double>(frequencyHz) / 1e9,
                                                  0, 'f', 6));
    painter.restore();
}

double maximumVswr(const std::vector<std::complex<double>>& trace)
{
    double result = 1.0;
    for (const auto& value : trace) {
        const double gamma = std::abs(value);
        if (!std::isfinite(gamma) || gamma >= 1.0) {
            return std::numeric_limits<double>::infinity();
        }
        result = std::max(result, (1.0 + gamma) / (1.0 - gamma));
    }
    return result;
}

void drawMagnitudeChart(QPainter& painter,
                        const QRectF& area,
                        const std::vector<std::uint64_t>& frequency,
                        const std::vector<std::complex<double>>& trace,
                        const TraceSummary& summary,
                        const std::vector<std::uint64_t>& markers,
                        const QColor& color)
{
    painter.save();
    painter.setPen(QPen(QColor(205, 214, 226), 1.0));
    painter.setBrush(Qt::white);
    painter.drawRoundedRect(area, 7, 7);

    const QRectF plot = area.adjusted(64, 45, -20, -43);
    double y_min = 0.0;
    double y_max = 0.0;
    bool have_value = false;
    std::vector<double> values;
    values.reserve(trace.size());
    for (const auto& value : trace) {
        const double db = magnitudeDb(value);
        values.push_back(db);
        if (!std::isfinite(db)) {
            continue;
        }
        y_min = have_value ? std::min(y_min, db) : db;
        y_max = have_value ? std::max(y_max, db) : db;
        have_value = true;
    }
    if (!have_value) {
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("Нет конечных данных"));
        painter.restore();
        return;
    }
    if (y_max <= y_min) {
        y_min -= 0.5;
        y_max += 0.5;
    }
    const double margin = std::max(0.5, (y_max - y_min) * 0.08);
    y_min -= margin;
    y_max += margin;

    painter.setFont(QFont(QStringLiteral("Sans Serif"), 7, QFont::Bold));
    painter.setPen(QColor(56, 68, 84));
    painter.drawText(QRectF(plot.left(), area.top() + 8, plot.width(), 18),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Модуль |S|, дБ"));
    painter.setPen(QPen(QColor(86, 105, 127), 1.0, Qt::DashLine));
    const double averageY = plot.bottom()
        - (summary.average_db - y_min) / (y_max - y_min) * plot.height();
    painter.drawLine(QPointF(plot.left(), averageY), QPointF(plot.right(), averageY));

    QColor grid(145, 158, 175, 85);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 7));
    for (int i = 0; i <= 8; ++i) {
        const double t = static_cast<double>(i) / 8.0;
        const double x = plot.left() + t * plot.width();
        painter.setPen(QPen(grid, 0.7, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.setPen(Qt::black);
        const double f_ghz = (static_cast<double>(frequency.front())
                              + t * static_cast<double>(frequency.back() - frequency.front())) / 1e9;
        painter.drawText(QRectF(x - 38, plot.bottom() + 4, 76, 16), Qt::AlignCenter,
                         QString::number(f_ghz, 'f', 4));
    }
    for (int i = 0; i <= 6; ++i) {
        const double t = static_cast<double>(i) / 6.0;
        const double y = plot.bottom() - t * plot.height();
        painter.setPen(QPen(grid, 0.7, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(Qt::black);
        painter.drawText(QRectF(area.left() + 2, y - 8, 48, 16), Qt::AlignRight,
                         QString::number(y_min + t * (y_max - y_min), 'f', 2));
    }
    painter.drawText(QRectF(plot.left(), plot.bottom() + 19, plot.width(), 14),
                     Qt::AlignCenter, QStringLiteral("Частота, ГГц"));

    QPolygonF line;
    line.reserve(static_cast<int>(values.size()));
    const double f0 = static_cast<double>(frequency.front());
    const double df = std::max(1.0, static_cast<double>(frequency.back() - frequency.front()));
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) {
            continue;
        }
        const double x = plot.left()
            + (static_cast<double>(frequency[i]) - f0) / df * plot.width();
        const double y = plot.bottom()
            - (values[i] - y_min) / (y_max - y_min) * plot.height();
        line << QPointF(x, y);
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.8));
    painter.drawPolyline(line);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::black);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(plot);

    const auto pointFor = [&](std::size_t index) {
        const double x = plot.left()
            + (static_cast<double>(frequency[index]) - f0) / df * plot.width();
        const double y = plot.bottom()
            - (values[index] - y_min) / (y_max - y_min) * plot.height();
        return QPointF(x, y);
    };
    const auto drawAutomaticMarker = [&](std::size_t index, const QString& label,
                                         const QColor& markerColor, bool below) {
        const QPointF point = pointFor(index);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(Qt::white, 1.0));
        painter.setBrush(markerColor);
        painter.drawEllipse(point, 4.5, 4.5);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setPen(markerColor.darker(130));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 6, QFont::Bold));
        QRectF labelArea(point.x() - 24, point.y() + (below ? 6 : -20), 48, 14);
        labelArea.moveLeft(std::clamp(labelArea.left(), plot.left(), plot.right() - 48));
        painter.drawText(labelArea, Qt::AlignCenter, label);
    };
    drawAutomaticMarker(summary.minimum_index, QStringLiteral("MIN"), QColor(205, 69, 69), false);
    drawAutomaticMarker(summary.maximum_index, QStringLiteral("MAX"), QColor(42, 146, 91), true);
    drawAutomaticMarker(summary.average_index, QStringLiteral("AVG"), QColor(47, 111, 190), false);

    painter.setFont(QFont(QStringLiteral("Sans Serif"), 6, QFont::Bold));
    for (std::size_t marker = 0; marker < markers.size(); ++marker) {
        if (markers[marker] < frequency.front() || markers[marker] > frequency.back()) {
            continue;
        }
        const std::size_t index = nearestFrequencyIndex(frequency, markers[marker]);
        const QPointF point = pointFor(index);
        painter.setPen(QPen(QColor(126, 74, 166), 0.9, Qt::DashLine));
        painter.drawLine(QPointF(point.x(), plot.top()), QPointF(point.x(), plot.bottom()));
        painter.setBrush(QColor(126, 74, 166));
        painter.drawEllipse(point, 3.5, 3.5);
        painter.drawText(QRectF(point.x() - 18, plot.top() + 2 + (marker % 2) * 12, 36, 12),
                         Qt::AlignCenter, QStringLiteral("M%1").arg(marker + 1));
    }
    painter.restore();
}

}  // namespace

bool validateTwoPortMeasurement(const TwoPortMeasurement& measurement,
                                std::string& diagnostics)
{
    diagnostics.clear();
    const auto expected = measurement.sweep.frequency_hz.size();
    if (expected < 2) {
        diagnostics = "two-port measurement: frequency axis has fewer than 2 points";
        return false;
    }
    if (std::adjacent_find(measurement.sweep.frequency_hz.begin(),
                           measurement.sweep.frequency_hz.end(),
                           std::greater_equal<>())
        != measurement.sweep.frequency_hz.end()) {
        diagnostics = "two-port measurement: frequency axis is not strictly increasing";
        return false;
    }
    constexpr std::array<const char*, 4> names{"S11", "S21", "S12", "S22"};
    for (int i = 0; i < static_cast<int>(names.size()); ++i) {
        const auto& trace = traceFor(measurement.sweep, i);
        if (trace.size() != expected) {
            diagnostics = std::string("two-port measurement: ") + names[static_cast<std::size_t>(i)]
                + " point count mismatch";
            return false;
        }
        for (const auto& value : trace) {
            if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) {
                diagnostics = std::string("two-port measurement: ")
                    + names[static_cast<std::size_t>(i)] + " contains NaN/Inf";
                return false;
            }
        }
    }
    if (!(measurement.reference_ohm > 0.0) || !std::isfinite(measurement.reference_ohm)) {
        diagnostics = "two-port measurement: invalid reference impedance";
        return false;
    }
    return true;
}

bool writeTouchstoneS2p(const std::filesystem::path& path,
                        const TwoPortMeasurement& measurement,
                        std::string& diagnostics)
{
    if (!validateTwoPortMeasurement(measurement, diagnostics)) {
        return false;
    }
    std::ostringstream body;
    body << "! AFAR RX Calibration Studio\n"
         << "! measured_utc=" << measurement.measured_utc << "\n"
         << "! vna_idn=" << measurement.vna_idn << "\n"
         << "! settings_source="
         << (measurement.applied_readback ? "instrument_readback" : "requested") << "\n"
         << "# Hz S RI R " << std::setprecision(12) << measurement.reference_ohm << "\n";
    body << std::scientific << std::setprecision(15);
    for (std::size_t i = 0; i < measurement.sweep.frequency_hz.size(); ++i) {
        body << measurement.sweep.frequency_hz[i];
        for (int trace = 0; trace < 4; ++trace) {
            const auto value = traceFor(measurement.sweep, trace)[i];
            body << ' ' << value.real() << ' ' << value.imag();
        }
        body << '\n';
    }

    QSaveFile file(pathToQString(path));
    if (!file.open(QIODevice::WriteOnly)) {
        diagnostics = "cannot open Touchstone file for writing";
        return false;
    }
    const std::string bytes = body.str();
    if (file.write(bytes.data(), static_cast<qint64>(bytes.size()))
            != static_cast<qint64>(bytes.size())
        || !file.commit()) {
        diagnostics = "Touchstone write/commit failed";
        return false;
    }
    return true;
}

bool writeTwoPortReportPdf(const std::filesystem::path& path,
                           const TwoPortMeasurement& measurement,
                           std::string& diagnostics)
{
    if (!validateTwoPortMeasurement(measurement, diagnostics)) {
        return false;
    }
    QSaveFile file(pathToQString(path));
    if (!file.open(QIODevice::WriteOnly)) {
        diagnostics = "cannot open PDF file for writing";
        return false;
    }
    {
        QPdfWriter writer(&file);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageOrientation(QPageLayout::Portrait);
        writer.setResolution(96);
        writer.setTitle(QStringLiteral("Протокол измерения S-параметров"));
        writer.setCreator(QStringLiteral("AFAR RX Calibration Studio"));

        QPainter painter(&writer);
        if (!painter.isActive()) {
            diagnostics = "cannot start PDF painter";
            return false;
        }
        const SweepConfig& settings = measurement.applied_readback
            ? measurement.applied : measurement.requested;
        const InstrumentInfo instrument = parseInstrument(measurement.vna_idn);
        constexpr std::array<const char*, 4> names{"S11", "S21", "S12", "S22"};
        const std::array<QColor, 4> colors{QColor(47, 128, 237), QColor(39, 174, 96),
                                          QColor(242, 153, 74), QColor(187, 107, 217)};
        constexpr int pageCount = 5;

        // Титульный лист: только идентификация и общие условия измерения.
        QRectF page = writer.pageLayout().paintRectPixels(writer.resolution());
        painter.fillRect(page, Qt::white);
        const QRectF banner(page.left(), page.top(), page.width(), 210);
        painter.fillRect(banner, QColor(25, 51, 82));
        painter.setPen(QColor(184, 210, 239));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 9, QFont::Bold));
        painter.drawText(banner.adjusted(34, 24, -34, -24),
                         Qt::AlignLeft | Qt::AlignTop,
                         QStringLiteral("AFAR RX CALIBRATION STUDIO"));
        painter.setPen(Qt::white);
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 23, QFont::Bold));
        painter.drawText(banner.adjusted(34, 66, -34, -28),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         QStringLiteral("ПРОТОКОЛ ИЗМЕРЕНИЯ\nS-ПАРАМЕТРОВ"));
        painter.setPen(QColor(213, 226, 242));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 10));
        painter.drawText(banner.adjusted(34, 166, -34, -18),
                         Qt::AlignLeft | Qt::AlignTop,
                         QStringLiteral("Полный двухпортовый свип S11 / S21 / S12 / S22"));

        painter.setPen(QColor(28, 57, 91));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 11, QFont::Bold));
        painter.drawText(QRectF(page.left() + 34, page.top() + 232, page.width() - 68, 24),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Дата и время измерения"));
        painter.setPen(QColor(30, 38, 49));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 14));
        painter.drawText(QRectF(page.left() + 34, page.top() + 260, page.width() - 68, 30),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         humanDateTime(measurement.measured_utc));

        const qreal left = page.left() + 34;
        const qreal width = page.width() - 68;
        const QString instrumentName = (instrument.vendor + QLatin1Char(' ') + instrument.model).trimmed();
        drawCard(painter, QRectF(left, page.top() + 310, width, 150),
                 QStringLiteral("Средство измерения"),
                 {
                     QStringLiteral("Анализатор цепей: %1")
                         .arg(instrumentName.isEmpty() ? QStringLiteral("не указан") : instrumentName),
                     QStringLiteral("Серийный номер: %1")
                         .arg(instrument.serial.isEmpty() ? QStringLiteral("не указан") : instrument.serial),
                     QStringLiteral("Версия ПО прибора: %1")
                         .arg(instrument.firmware.isEmpty() ? QStringLiteral("не указана") : instrument.firmware),
                     QStringLiteral("Идентификация *IDN?: %1")
                         .arg(QString::fromStdString(measurement.vna_idn))
                 });
        drawCard(painter, QRectF(left, page.top() + 475, width, 160),
                 QStringLiteral("Условия измерения"),
                 {
                     QStringLiteral("Диапазон частот: %1 - %2 ГГц")
                         .arg(static_cast<double>(measurement.sweep.frequency_hz.front()) / 1e9, 0, 'f', 6)
                         .arg(static_cast<double>(measurement.sweep.frequency_hz.back()) / 1e9, 0, 'f', 6),
                     QStringLiteral("Точек: %1; полоса ПЧ: %2 Гц")
                         .arg(static_cast<qulonglong>(measurement.sweep.frequency_hz.size()))
                         .arg(static_cast<qulonglong>(settings.ifbw_hz)),
                     QStringLiteral("Мощность: %1 дБм; усреднений: %2")
                         .arg(settings.power_dbm, 0, 'f', 1)
                         .arg(static_cast<qulonglong>(settings.averages)),
                     QStringLiteral("Опорное сопротивление: %1 Ом; источник настроек: %2")
                         .arg(measurement.reference_ohm, 0, 'f', 1)
                         .arg(measurement.applied_readback
                                  ? QStringLiteral("считано с прибора")
                                  : QStringLiteral("задано оператором")),
                     QStringLiteral("Частотная ось: общая для всех четырёх трасс")
                 });
        drawCard(painter, QRectF(left, page.top() + 650, width, 125),
                 QStringLiteral("Состав протокола"),
                 {
                     QStringLiteral("Страницы 2-5: по одному полноразмерному графику на страницу"),
                     QStringLiteral("Для каждой трассы: MIN, MAX, AVG, размах, фаза и параметры свипа"),
                     measurement.marker_frequency_hz.empty()
                         ? QStringLiteral("Пользовательские маркеры: не установлены")
                         : QStringLiteral("Пользовательских маркеров: %1")
                               .arg(static_cast<qulonglong>(measurement.marker_frequency_hz.size())),
                     QStringLiteral("Формат исходных комплексных данных: Touchstone S2P, RI")
                 });
        painter.setPen(QColor(102, 112, 125));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
        painter.drawText(QRectF(left, page.top() + 798, width, 58),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         QStringLiteral("Протокол сформирован автоматически из одного согласованного "
                                        "набора комплексных отсчётов. Повторное измерение при экспорте "
                                        "не выполнялось."));
        drawFooter(painter, page, 1, pageCount);

        for (int traceIndex = 0; traceIndex < 4; ++traceIndex) {
            writer.newPage();
            page = writer.pageLayout().paintRectPixels(writer.resolution());
            painter.fillRect(page, Qt::white);
            const auto& trace = traceFor(measurement.sweep, traceIndex);
            const TraceSummary summary = summarize(trace);
            const auto phase = unwrappedPhaseDeg(trace);
            const QString name = QString::fromLatin1(names[static_cast<std::size_t>(traceIndex)]);
            const QColor color = colors[static_cast<std::size_t>(traceIndex)];

            painter.fillRect(QRectF(page.left() + 24, page.top() + 16, 6, 46), color);
            painter.setPen(QColor(24, 34, 48));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 18, QFont::Bold));
            painter.drawText(QRectF(page.left() + 44, page.top() + 14, page.width() - 70, 28),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("%1 - модуль и контрольные значения").arg(name));
            painter.setPen(QColor(91, 102, 116));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
            painter.drawText(QRectF(page.left() + 44, page.top() + 43, page.width() - 70, 18),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             traceDescription(traceIndex));

            const QRectF chart(page.left() + 24, page.top() + 76, page.width() - 48, 445);
            drawMagnitudeChart(painter, chart, measurement.sweep.frequency_hz, trace,
                               summary, measurement.marker_frequency_hz, color);

            const qreal metricsTop = page.top() + 535;
            const qreal metricsGap = 10;
            const qreal metricWidth = (page.width() - 48 - metricsGap * 2) / 3.0;
            drawMetricCard(painter,
                           QRectF(page.left() + 24, metricsTop, metricWidth, 76),
                           QStringLiteral("MIN"), summary.minimum_db,
                           measurement.sweep.frequency_hz[summary.minimum_index],
                           QColor(205, 69, 69));
            drawMetricCard(painter,
                           QRectF(page.left() + 24 + metricWidth + metricsGap,
                                  metricsTop, metricWidth, 76),
                           QStringLiteral("MAX"), summary.maximum_db,
                           measurement.sweep.frequency_hz[summary.maximum_index],
                           QColor(42, 146, 91));
            drawMetricCard(painter,
                           QRectF(page.left() + 24 + (metricWidth + metricsGap) * 2,
                                  metricsTop, metricWidth, 76),
                           QStringLiteral("СРЕДНЕЕ"), summary.average_db,
                           measurement.sweep.frequency_hz[summary.average_index],
                           QColor(47, 111, 190));

            const auto phaseMinMax = std::minmax_element(phase.begin(), phase.end());
            QString extra = QStringLiteral("Размах |S|: %1 дБ; фаза unwrap: %2 ... %3 град.; "
                                           "отсчётов: %4")
                                .arg(summary.maximum_db - summary.minimum_db, 0, 'f', 3)
                                .arg(*phaseMinMax.first, 0, 'f', 2)
                                .arg(*phaseMinMax.second, 0, 'f', 2)
                                .arg(static_cast<qulonglong>(trace.size()));
            if (traceIndex == 0 || traceIndex == 3) {
                const double vswr = maximumVswr(trace);
                extra += QStringLiteral("; максимальный КСВН: %1")
                    .arg(std::isfinite(vswr) ? QString::number(vswr, 'f', 3)
                                             : QStringLiteral("не ограничен (|S| >= 1)"));
            }
            painter.setPen(QColor(55, 65, 78));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
            painter.drawText(QRectF(page.left() + 28, page.top() + 622,
                                    page.width() - 56, 36),
                             Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, extra);
            painter.setPen(QColor(92, 103, 117));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 7));
            painter.drawText(QRectF(page.left() + 28, page.top() + 656,
                                    page.width() - 56, 18),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Свип: %1 - %2 ГГц | IFBW %3 Гц | %4 дБм | AVG %5 | R %6 Ом")
                                 .arg(static_cast<double>(measurement.sweep.frequency_hz.front()) / 1e9,
                                      0, 'f', 6)
                                 .arg(static_cast<double>(measurement.sweep.frequency_hz.back()) / 1e9,
                                      0, 'f', 6)
                                 .arg(static_cast<qulonglong>(settings.ifbw_hz))
                                 .arg(settings.power_dbm, 0, 'f', 1)
                                 .arg(static_cast<qulonglong>(settings.averages))
                                 .arg(measurement.reference_ohm, 0, 'f', 1));

            const qreal tableTop = page.top() + 690;
            painter.setPen(QColor(28, 57, 91));
            painter.setFont(QFont(QStringLiteral("Sans Serif"), 9, QFont::Bold));
            painter.drawText(QRectF(page.left() + 24, tableTop, page.width() - 48, 20),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Пользовательские маркеры"));
            std::vector<std::pair<std::size_t, std::size_t>> markerRows;
            for (std::size_t marker = 0; marker < measurement.marker_frequency_hz.size(); ++marker) {
                const auto frequency = measurement.marker_frequency_hz[marker];
                if (frequency >= measurement.sweep.frequency_hz.front()
                    && frequency <= measurement.sweep.frequency_hz.back()) {
                    markerRows.emplace_back(marker, nearestFrequencyIndex(
                                                       measurement.sweep.frequency_hz, frequency));
                }
            }
            if (markerRows.empty()) {
                painter.setPen(QColor(110, 120, 133));
                painter.setBrush(QColor(247, 249, 252));
                painter.drawRoundedRect(QRectF(page.left() + 24, tableTop + 27,
                                               page.width() - 48, 44), 5, 5);
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
                painter.drawText(QRectF(page.left() + 38, tableTop + 27,
                                        page.width() - 76, 44),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("Маркеры оператором не устанавливались."));
            } else {
                const qreal tableLeft = page.left() + 24;
                const qreal tableWidth = page.width() - 48;
                const qreal headerTop = tableTop + 27;
                const qreal rowHeight = std::max<qreal>(11.0,
                    std::min<qreal>(19.0, (page.bottom() - 46 - headerTop - 22)
                                             / static_cast<qreal>(markerRows.size())));
                const std::array<qreal, 6> columns{
                    tableLeft, tableLeft + tableWidth * 0.10, tableLeft + tableWidth * 0.34,
                    tableLeft + tableWidth * 0.56, tableLeft + tableWidth * 0.78,
                    tableLeft + tableWidth
                };
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(225, 235, 247));
                painter.drawRect(QRectF(tableLeft, headerTop, tableWidth, 22));
                painter.setPen(QColor(39, 52, 69));
                painter.setFont(QFont(QStringLiteral("Sans Serif"), 7, QFont::Bold));
                const std::array<QString, 5> headers{
                    QStringLiteral("Маркер"), QStringLiteral("Частота, ГГц"),
                    QStringLiteral("Модуль, дБ"), QStringLiteral("Фаза, град."),
                    (traceIndex == 0 || traceIndex == 3)
                        ? QStringLiteral("КСВН") : QStringLiteral("|S| лин.")
                };
                for (int column = 0; column < 5; ++column) {
                    painter.drawText(QRectF(columns[static_cast<std::size_t>(column)], headerTop,
                                            columns[static_cast<std::size_t>(column + 1)]
                                                - columns[static_cast<std::size_t>(column)], 22),
                                     Qt::AlignCenter, headers[static_cast<std::size_t>(column)]);
                }
                painter.setFont(QFont(QStringLiteral("Sans Serif"), rowHeight < 15 ? 6 : 7));
                for (std::size_t row = 0; row < markerRows.size(); ++row) {
                    const std::size_t marker = markerRows[row].first;
                    const std::size_t point = markerRows[row].second;
                    const double linear = std::abs(trace[point]);
                    const double vswr = linear < 1.0
                        ? (1.0 + linear) / (1.0 - linear)
                        : std::numeric_limits<double>::infinity();
                    const std::array<QString, 5> cells{
                        QStringLiteral("M%1").arg(marker + 1),
                        QString::number(static_cast<double>(measurement.sweep.frequency_hz[point]) / 1e9,
                                        'f', 6),
                        QString::number(magnitudeDb(trace[point]), 'f', 3),
                        QString::number(phase[point], 'f', 2),
                        (traceIndex == 0 || traceIndex == 3)
                            ? (std::isfinite(vswr) ? QString::number(vswr, 'f', 3)
                                                   : QStringLiteral("не ограничен"))
                            : QString::number(linear, 'f', 6)
                    };
                    const qreal rowTop = headerTop + 22 + rowHeight * static_cast<qreal>(row);
                    if (row % 2 == 1) {
                        painter.fillRect(QRectF(tableLeft, rowTop, tableWidth, rowHeight),
                                         QColor(248, 250, 253));
                    }
                    painter.setPen(QColor(52, 61, 73));
                    for (int column = 0; column < 5; ++column) {
                        painter.drawText(QRectF(columns[static_cast<std::size_t>(column)], rowTop,
                                                columns[static_cast<std::size_t>(column + 1)]
                                                    - columns[static_cast<std::size_t>(column)],
                                                rowHeight),
                                         Qt::AlignCenter, cells[static_cast<std::size_t>(column)]);
                    }
                }
            }
            drawFooter(painter, page, traceIndex + 2, pageCount);
        }
        painter.end();
    }
    if (!file.commit()) {
        diagnostics = "PDF commit failed";
        return false;
    }
    return true;
}

}  // namespace afar::report
