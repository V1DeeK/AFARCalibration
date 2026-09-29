#include "TwoPortExport.h"

#include <QFile>
#include <QFont>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>

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
    double minimum_db{0.0};
    double maximum_db{0.0};
    double average_db{0.0};
};

TraceSummary summarize(const std::vector<std::complex<double>>& trace)
{
    TraceSummary result;
    bool first = true;
    long double sum = 0.0;
    std::size_t count = 0;
    for (const auto& value : trace) {
        const double db = magnitudeDb(value);
        if (!std::isfinite(db)) {
            continue;
        }
        if (first) {
            result.minimum_db = db;
            result.maximum_db = db;
            first = false;
        } else {
            result.minimum_db = std::min(result.minimum_db, db);
            result.maximum_db = std::max(result.maximum_db, db);
        }
        sum += db;
        ++count;
    }
    if (count > 0) {
        result.average_db = static_cast<double>(sum / count);
    }
    return result;
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
                        const QString& name,
                        const std::vector<std::uint64_t>& frequency,
                        const std::vector<std::complex<double>>& trace,
                        const QColor& color)
{
    painter.save();
    painter.setPen(QPen(Qt::black, 1.0));
    painter.setBrush(Qt::white);
    painter.drawRect(area);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 10, QFont::Bold));
    painter.drawText(area.adjusted(8, 4, -8, -4), Qt::AlignLeft | Qt::AlignTop,
                     name + QStringLiteral(" — модуль, дБ"));

    const QRectF plot = area.adjusted(54, 28, -16, -38);
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

    QColor grid(150, 150, 150, 100);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 7));
    for (int i = 0; i <= 5; ++i) {
        const double t = static_cast<double>(i) / 5.0;
        const double x = plot.left() + t * plot.width();
        const double y = plot.bottom() - t * plot.height();
        painter.setPen(QPen(grid, 0.7, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(Qt::black);
        const double f_ghz = (static_cast<double>(frequency.front())
                              + t * static_cast<double>(frequency.back() - frequency.front())) / 1e9;
        painter.drawText(QRectF(x - 38, plot.bottom() + 4, 76, 16), Qt::AlignCenter,
                         QString::number(f_ghz, 'f', 4));
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
        writer.setTitle(QStringLiteral("Отчёт двухпортового измерения"));
        writer.setCreator(QStringLiteral("AFAR RX Calibration Studio"));

        QPainter painter(&writer);
        if (!painter.isActive()) {
            diagnostics = "cannot start PDF painter";
            return false;
        }
        const QRect page = writer.pageLayout().paintRectPixels(writer.resolution());
        int y = page.top() + 20;
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 16, QFont::Bold));
        painter.drawText(page.adjusted(20, 0, -20, 0), Qt::AlignHCenter | Qt::AlignTop,
                         QStringLiteral("Отчёт измерения двухпортового устройства"));
        y += 44;
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 9));
        auto line = [&](const QString& text) {
            painter.drawText(page.left() + 28, y, text);
            y += 18;
        };
        line(QStringLiteral("Дата UTC: %1").arg(QString::fromStdString(measurement.measured_utc)));
        line(QStringLiteral("VNA: %1").arg(QString::fromStdString(measurement.vna_idn)));
        line(QStringLiteral("Диапазон: %1 … %2 ГГц")
                 .arg(static_cast<double>(measurement.sweep.frequency_hz.front()) / 1e9, 0, 'f', 6)
                 .arg(static_cast<double>(measurement.sweep.frequency_hz.back()) / 1e9, 0, 'f', 6));
        const SweepConfig& settings = measurement.applied_readback
            ? measurement.applied : measurement.requested;
        line(QStringLiteral("Настройки: %1")
                 .arg(measurement.applied_readback
                          ? QStringLiteral("считаны обратно с прибора")
                          : QStringLiteral("запрошенные; readback недоступен")));
        line(QStringLiteral("Точки: %1; IFBW: %2 Гц; мощность: %3 дБм; усреднение: %4; R: %5 Ом")
                 .arg(measurement.sweep.frequency_hz.size())
                 .arg(settings.ifbw_hz)
                 .arg(settings.power_dbm, 0, 'f', 1)
                 .arg(settings.averages)
                 .arg(measurement.reference_ohm, 0, 'f', 1));
        y += 10;
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 10, QFont::Bold));
        painter.drawText(page.left() + 28, y, QStringLiteral("Сводные значения модуля"));
        y += 24;
        painter.setFont(QFont(QStringLiteral("Monospace"), 9));
        constexpr std::array<const char*, 4> names{"S11", "S21", "S12", "S22"};
        for (int i = 0; i < 4; ++i) {
            const auto summary = summarize(traceFor(measurement.sweep, i));
            line(QStringLiteral("%1: min %2 дБ; max %3 дБ; среднее %4 дБ")
                     .arg(QString::fromLatin1(names[static_cast<std::size_t>(i)]), 4)
                     .arg(summary.minimum_db, 8, 'f', 3)
                     .arg(summary.maximum_db, 8, 'f', 3)
                     .arg(summary.average_db, 8, 'f', 3));
        }
        const double vswr1 = maximumVswr(measurement.sweep.s11);
        const double vswr2 = maximumVswr(measurement.sweep.s22);
        line(QStringLiteral("Максимальный КСВН: порт 1 — %1; порт 2 — %2")
                 .arg(std::isfinite(vswr1) ? QString::number(vswr1, 'f', 3)
                                          : QStringLiteral("∞"))
                 .arg(std::isfinite(vswr2) ? QString::number(vswr2, 'f', 3)
                                          : QStringLiteral("∞")));

        writer.newPage();
        const QRect chartsPage = writer.pageLayout().paintRectPixels(writer.resolution());
        const qreal gap = 14.0;
        const qreal margin = 18.0;
        const qreal chartWidth = (chartsPage.width() - margin * 2.0 - gap) / 2.0;
        const qreal chartHeight = (chartsPage.height() - margin * 2.0 - gap) / 2.0;
        const std::array<QColor, 4> colors{QColor(47, 128, 237), QColor(39, 174, 96),
                                          QColor(242, 153, 74), QColor(187, 107, 217)};
        for (int i = 0; i < 4; ++i) {
            const int row = i / 2;
            const int col = i % 2;
            const QRectF area(chartsPage.left() + margin + col * (chartWidth + gap),
                              chartsPage.top() + margin + row * (chartHeight + gap),
                              chartWidth, chartHeight);
            drawMagnitudeChart(painter, area,
                               QString::fromLatin1(names[static_cast<std::size_t>(i)]),
                               measurement.sweep.frequency_hz,
                               traceFor(measurement.sweep, i), colors[static_cast<std::size_t>(i)]);
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
