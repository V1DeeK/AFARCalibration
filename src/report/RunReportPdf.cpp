#include "RunReportPdf.h"

#if __has_include("AfarBuildInfo.h")
#include "AfarBuildInfo.h"
#endif

#include <nlohmann/json.hpp>

#include <QFileInfo>
#include <QFont>
#include <QFontMetricsF>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <limits>
#include <string_view>

#ifndef AFAR_SOFTWARE_VERSION
#define AFAR_SOFTWARE_VERSION "0.1.0"
#endif

#ifndef AFAR_CXX_COMPILER_ID
#define AFAR_CXX_COMPILER_ID "unknown"
#endif

#ifndef AFAR_CXX_COMPILER_VERSION
#define AFAR_CXX_COMPILER_VERSION "unknown"
#endif

#ifndef AFAR_CXX_STANDARD
#define AFAR_CXX_STANDARD "20"
#endif

#ifndef AFAR_QT_VERSION
#define AFAR_QT_VERSION "unknown"
#endif

#ifndef AFAR_NLOHMANN_JSON_VERSION
#define AFAR_NLOHMANN_JSON_VERSION "unknown"
#endif

// Пустая строка: git не был доступен при конфигурации CMake.
#ifndef AFAR_GIT_COMMIT
#define AFAR_GIT_COMMIT ""
#endif

namespace afar::report {
namespace {

QString qString(const std::string& text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString softwareVersion(const RunReportInfo& info)
{
    return info.software_version.empty() ? QStringLiteral(AFAR_SOFTWARE_VERSION)
                                         : qString(info.software_version);
}

QString buildCommit()
{
    const std::string_view hash{AFAR_GIT_COMMIT};
    return hash.empty() ? QStringLiteral("хеш не вшит")
                        : QString::fromLatin1(hash.data(), static_cast<qsizetype>(hash.size()));
}

QString makeThruSummary(const RunReportInfo& info)
{
    if (info.thru_measured_mag_db >= 0.0 && info.thru_measured_phase_deg >= 0.0) {
        return QStringLiteral("%1 дБ / %2° (предел %3 дБ / %4°)")
            .arg(info.thru_measured_mag_db, 0, 'g', 6)
            .arg(info.thru_measured_phase_deg, 0, 'g', 6)
            .arg(info.thru_limit_mag_db, 0, 'g', 6)
            .arg(info.thru_limit_phase_deg, 0, 'g', 6);
    }
    return QStringLiteral("не измерен / значения мастера");
}

void drawWrappedLine(QPainter& painter, const QRectF& page, qreal& y, const QString& text)
{
    const QRectF bounds(page.left() + 28, y, page.width() - 56, page.bottom() - y - 28);
    const QRectF used = painter.boundingRect(bounds, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                                              text);
    painter.drawText(bounds, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
    y += used.height() + 6;
}

void drawFooter(QPainter& painter, const QRectF& page, int number)
{
    painter.save();
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
    painter.setPen(QColor(90, 90, 90));
    painter.drawText(page.adjusted(28, 0, -28, -10), Qt::AlignRight | Qt::AlignBottom,
                     QStringLiteral("Страница %1").arg(number));
    painter.restore();
}

void drawChannelHeader(QPainter& painter, const QRectF& page, const QString& title,
                       bool demoMode, int pageNumber, int pageCount)
{
    painter.save();
    painter.setClipping(false);
    const QRectF headerArea(page.left(), page.top(), page.width(), 68);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(25, 51, 82));
    painter.drawRoundedRect(headerArea, 4, 4);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 18, QFont::Bold));
    painter.drawText(headerArea.adjusted(28, 15, -260, -15),
                     Qt::AlignLeft | Qt::AlignVCenter, title);
    const QRectF badgeArea(headerArea.right() - 230, headerArea.top() + 18, 202, 30);
    painter.setPen(Qt::NoPen);
    painter.setBrush(demoMode ? QColor(181, 43, 43) : QColor(33, 126, 78));
    painter.drawRoundedRect(badgeArea, 5, 5);
    painter.setPen(Qt::white);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::Bold));
    painter.drawText(badgeArea, Qt::AlignCenter,
                     demoMode ? QStringLiteral("DEMO - НЕ МЕТРОЛОГИЯ")
                              : QStringLiteral("LIVE - ЖИВОЙ VNA"));
    painter.setPen(QColor(90, 100, 115));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 7));
    painter.drawText(QRectF(page.left() + 28, page.bottom() - 22, page.width() - 56, 14),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("Страница %1 из %2").arg(pageNumber).arg(pageCount));
    painter.restore();
}

void drawInfoCard(QPainter& painter, const QRectF& area, const QString& title,
                  const QStringList& lines)
{
    painter.setPen(QPen(QColor(211, 219, 230), 1.0));
    painter.setBrush(QColor(247, 249, 252));
    painter.drawRoundedRect(area, 7, 7);
    painter.setPen(QColor(28, 57, 91));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 10, QFont::Bold));
    painter.drawText(area.adjusted(14, 10, -14, -10), Qt::AlignLeft | Qt::AlignTop, title);
    painter.setPen(QColor(40, 48, 60));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
    qreal y = area.top() + 34;
    for (const auto& line : lines) {
        painter.drawText(QRectF(area.left() + 14, y, area.width() - 28, 18),
                         Qt::AlignLeft | Qt::AlignVCenter, line);
        y += 20;
    }
}

QColor blend(const QColor& from, const QColor& to, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * t,
                            from.greenF() + (to.greenF() - from.greenF()) * t,
                            from.blueF() + (to.blueF() - from.blueF()) * t);
}

template <typename Value>
void drawChannelMatrix(QPainter& painter, const QRectF& page,
                       const ChannelReportInfo& channel, const QString& title,
                       const QString& legendTitle, Value value, bool statusMatrix,
                       bool symmetricScale)
{
    painter.save();
    const std::size_t rows = channel.att_codes.size();
    const std::size_t columns = channel.phase_codes.size();
    if (rows == 0 || columns == 0 || channel.states.size() != rows * columns) {
        painter.setPen(Qt::black);
        painter.drawText(page, Qt::AlignCenter, QStringLiteral("Нет данных матрицы"));
        painter.restore();
        return;
    }
    painter.setPen(QColor(28, 57, 91));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 11, QFont::Bold));
    painter.drawText(QRectF(page.left() + 28, page.top() + 78, page.width() - 56, 24),
                     Qt::AlignLeft | Qt::AlignVCenter, title);

    const qreal availableHeight = page.height() - 330;
    const qreal gridSize = std::min<qreal>(availableHeight, page.width() - 164);
    const QRectF grid(page.left() + 82, page.top() + 118, gridSize, gridSize);
    const qreal cellWidth = grid.width() / static_cast<qreal>(columns);
    const qreal cellHeight = grid.height() / static_cast<qreal>(rows);
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    if (!statusMatrix) {
        for (const auto& state : channel.states) {
            const double sample = value(state);
            if (state.center_value_available && std::isfinite(sample)) {
                minimum = std::min(minimum, sample);
                maximum = std::max(maximum, sample);
            }
        }
        if (symmetricScale && std::isfinite(minimum) && std::isfinite(maximum)) {
            maximum = std::max(std::fabs(minimum), std::fabs(maximum));
            minimum = -maximum;
        }
        if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
            minimum = 0.0;
            maximum = 1.0;
        }
        if (maximum <= minimum) {
            minimum -= 0.5;
            maximum += 0.5;
        }
    }
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            const auto& state = channel.states[row * columns + column];
            QColor color;
            if (statusMatrix) {
                color = !state.completed ? QColor(190, 196, 204)
                    : state.valid ? QColor(54, 162, 92) : QColor(207, 66, 66);
            } else if (!state.center_value_available) {
                color = QColor(190, 196, 204);
            } else {
                const double t = (value(state) - minimum) / (maximum - minimum);
                if (symmetricScale) {
                    color = t < 0.5 ? blend(QColor(44, 112, 196), Qt::white, t * 2.0)
                                    : blend(Qt::white, QColor(204, 55, 55), (t - 0.5) * 2.0);
                } else {
                    color = t < 0.5 ? blend(QColor(32, 98, 171), QColor(242, 240, 157), t * 2.0)
                                    : blend(QColor(242, 240, 157), QColor(181, 43, 43),
                                            (t - 0.5) * 2.0);
                }
            }
            painter.fillRect(QRectF(grid.left() + column * cellWidth,
                                    grid.top() + row * cellHeight,
                                    std::ceil(cellWidth), std::ceil(cellHeight)), color);
        }
    }
    painter.setPen(QPen(QColor(35, 45, 58), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(grid);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 6));
    const std::size_t rowStep = std::max<std::size_t>(1, rows / 8);
    const std::size_t columnStep = std::max<std::size_t>(1, columns / 8);
    for (std::size_t row = 0; row < rows; row += rowStep) {
        painter.drawText(QRectF(grid.left() - 48, grid.top() + row * cellHeight - 6, 42, 13),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(channel.att_codes[row]));
    }
    for (std::size_t column = 0; column < columns; column += columnStep) {
        painter.save();
        painter.translate(grid.left() + column * cellWidth + cellWidth / 2, grid.top() - 6);
        painter.rotate(-55);
        painter.drawText(QRectF(0, -8, 42, 12), Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(channel.phase_codes[column]));
        painter.restore();
    }
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::Bold));
    painter.drawText(QRectF(grid.left(), grid.bottom() + 12, grid.width(), 18),
                     Qt::AlignCenter, QStringLiteral("Код фазы"));
    painter.save();
    painter.translate(grid.left() - 62, grid.center().y());
    painter.rotate(-90);
    painter.drawText(QRectF(-80, -10, 160, 18), Qt::AlignCenter,
                     QStringLiteral("Код аттенюатора"));
    painter.restore();

    const qreal legendLeft = grid.left();
    const qreal legendTop = grid.bottom() + 45;
    painter.setPen(QColor(28, 57, 91));
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 9, QFont::Bold));
    painter.drawText(QRectF(legendLeft, legendTop, grid.width(), 20),
                     Qt::AlignLeft, legendTitle);
    painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
    if (statusMatrix) {
        const std::array<std::pair<QColor, QString>, 3> legend{{
            {QColor(54, 162, 92), QStringLiteral("completed / valid")},
            {QColor(207, 66, 66), QStringLiteral("completed / error")},
            {QColor(190, 196, 204), QStringLiteral("не завершено")}
        }};
        for (std::size_t index = 0; index < legend.size(); ++index) {
            const qreal itemLeft = legendLeft + index * (grid.width() / 3.0);
            painter.fillRect(QRectF(itemLeft, legendTop + 30, 20, 20),
                             legend[index].first);
            painter.setPen(QColor(45, 52, 62));
            painter.drawText(QRectF(itemLeft + 28, legendTop + 30,
                                    grid.width() / 3.0 - 32, 20),
                             Qt::AlignLeft | Qt::AlignVCenter, legend[index].second);
        }
    } else {
        const QRectF scale(legendLeft, legendTop + 30, grid.width() * 0.65, 24);
        for (int pixel = 0; pixel < static_cast<int>(scale.width()); ++pixel) {
            const double t = pixel / scale.width();
            const QColor color = symmetricScale
                ? (t < 0.5 ? blend(QColor(44, 112, 196), Qt::white, t * 2.0)
                           : blend(Qt::white, QColor(204, 55, 55), (t - 0.5) * 2.0))
                : (t < 0.5 ? blend(QColor(32, 98, 171), QColor(242, 240, 157), t * 2.0)
                           : blend(QColor(242, 240, 157), QColor(181, 43, 43),
                                   (t - 0.5) * 2.0));
            painter.fillRect(QRectF(scale.left() + pixel, scale.top(), 1, scale.height()), color);
        }
        painter.setPen(QColor(45, 52, 62));
        painter.drawRect(scale);
        painter.drawText(QRectF(scale.left(), scale.bottom() + 3, 100, 18),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QString::number(minimum, 'f', 3));
        painter.drawText(QRectF(scale.right() - 100, scale.bottom() + 3, 100, 18),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(maximum, 'f', 3));
        painter.drawText(QRectF(scale.right() + 20, scale.top() - 4,
                                grid.right() - scale.right() - 20, 42),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         QStringLiteral("Серый: нет пригодного значения центрального среза"));
    }
    painter.restore();
}

bool writeDetailedChannelReportPdf(const std::filesystem::path& path,
                                   const RunReportInfo& info,
                                   std::string& diagnostics)
{
    constexpr int pageCount = 5;
    const auto& channel = info.channels.front();
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly)) {
        diagnostics = "cannot write channel pdf: " + path.string();
        return false;
    }
    {
        QPdfWriter writer(&file);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageOrientation(QPageLayout::Portrait);
        writer.setPageMargins(QMarginsF(8, 8, 8, 8), QPageLayout::Millimeter);
        writer.setResolution(96);
        writer.setTitle(QStringLiteral("Отчёт канала %1 - %2")
                            .arg(channel.channel).arg(qString(info.run_id)));
        writer.setCreator(QStringLiteral("AFAR RX Calibration Studio"));
        QPainter painter(&writer);
        if (!painter.isActive()) {
            diagnostics = "cannot start channel PDF painter";
            return false;
        }

        QRectF page = writer.pageLayout().paintRectPixels(writer.resolution());
        const QString channelHeader = QStringLiteral("КАНАЛ %1 - ОТЧЁТ КАЛИБРОВКИ")
                                          .arg(channel.channel);
        drawChannelHeader(painter, page, channelHeader, info.demo_mode, 1, pageCount);
        const bool passed = channelReportPasses(channel);
        const QColor statusColor = passed ? QColor(33, 126, 78) : QColor(181, 43, 43);
        painter.setPen(Qt::NoPen);
        painter.setBrush(statusColor);
        const QRectF statusArea(page.left() + 28, page.top() + 88, page.width() - 56, 72);
        painter.drawRoundedRect(statusArea, 7, 7);
        painter.setPen(Qt::white);
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 11, QFont::Bold));
        painter.drawText(statusArea.adjusted(18, 8, -18, -34),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         passed ? QStringLiteral("PASS - ПОЛНОЕ ПОКРЫТИЕ, ОШИБОК НЕТ")
                                : QStringLiteral("FAIL / ПРОВЕРИТЬ - ЕСТЬ ПРОПУСКИ ИЛИ ОШИБКИ"));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 9, QFont::Bold));
        painter.drawText(statusArea.adjusted(18, 36, -18, -8),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("Покрытие %1/%2 состояний")
                             .arg(channel.completed_states).arg(channel.expected_states));

        const qreal half = (page.width() - 76) / 2.0;
        drawInfoCard(painter, QRectF(page.left() + 28, page.top() + 180, half, 190),
                     QStringLiteral("Серия и VNA"),
                     {
                         QStringLiteral("Серия: %1; канал: %2")
                             .arg(qString(info.run_id)).arg(channel.channel),
                         QStringLiteral("Источник: %1")
                             .arg(info.demo_mode ? QStringLiteral("DEMO - не метрология")
                                                 : QStringLiteral("LIVE")),
                         QStringLiteral("VNA: %1").arg(qString(info.vna_idn)),
                         QStringLiteral("Диапазон: %1 - %2 ГГц; точек: %3")
                             .arg(static_cast<double>(info.f_start_hz) / 1e9, 0, 'f', 6)
                             .arg(static_cast<double>(info.f_stop_hz) / 1e9, 0, 'f', 6)
                             .arg(info.frequency_points),
                         QStringLiteral("IFBW %1 Гц; мощность %2 дБм; AVG %3")
                             .arg(info.ifbw_hz).arg(info.power_dbm, 0, 'f', 1)
                             .arg(info.averages),
                         QStringLiteral("Центральный срез: %1 ГГц")
                             .arg(static_cast<double>(channel.center_frequency_hz) / 1e9,
                                  0, 'f', 6)
                     });
        drawInfoCard(painter, QRectF(page.left() + 48 + half, page.top() + 180, half, 190),
                     QStringLiteral("Покрытие и качество"),
                     {
                         QStringLiteral("Состояния: valid %1; error %2; не завершено %3")
                             .arg(channel.valid_states).arg(channel.error_states)
                             .arg(channel.expected_states - channel.completed_states),
                         QStringLiteral("Точки direct LUT: valid %1; invalid %2")
                             .arg(channel.valid_direct_count).arg(channel.invalid_direct_count),
                         QStringLiteral("Макс. ошибка: ослабление %1 дБ; фаза %2°")
                             .arg(channel.max_atten_error_db, 0, 'f', 4)
                             .arg(channel.max_phase_error_deg, 0, 'f', 4),
                         QStringLiteral("Repeatability: %1 дБ / %2°")
                             .arg(channel.max_repeatability_db, 0, 'f', 4)
                             .arg(channel.max_repeatability_deg, 0, 'f', 4),
                         QStringLiteral("Макс. drift: %1°; overload состояний: %2")
                             .arg(channel.max_drift_phase_deg, 0, 'f', 4)
                             .arg(channel.overload_states),
                         QStringLiteral("Повторных попыток: %1; inverse valid: %2")
                             .arg(channel.retry_count).arg(channel.valid_inverse_count)
                     });
        drawInfoCard(painter, QRectF(page.left() + 28, page.top() + 390,
                                     page.width() - 56, 195),
                     QStringLiteral("Файлы и SHA-256"),
                     {
                         QStringLiteral("Калибровка AFARPQ: %1")
                             .arg(qString(channel.calibration_filename)),
                         QStringLiteral("SHA-256: %1")
                             .arg(qString(channel.calibration_sha256)),
                         QStringLiteral("Калибровка CSV: %1")
                             .arg(qString(channel.calibration_csv_filename)),
                         QStringLiteral("SHA-256: %1")
                             .arg(qString(channel.calibration_csv_sha256)),
                         QStringLiteral("Сырые данные: %1")
                             .arg(qString(channel.raw_filename)),
                         QStringLiteral("SHA-256: %1")
                             .arg(qString(channel.raw_sha256)),
                         QStringLiteral("Каталог серии: %1").arg(qString(info.series_path))
                     });
        painter.setPen(QColor(110, 70, 35));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
        painter.drawText(QRectF(page.left() + 28, page.top() + 608, page.width() - 56, 38),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         QStringLiteral("Пороговые значения являются настройками ПО. "
                                        "Полные точки сохранены в RAW и LUT; PDF содержит сводку."));

        writer.newPage();
        page = writer.pageLayout().paintRectPixels(writer.resolution());
        drawChannelHeader(painter, page, channelHeader, info.demo_mode, 2, pageCount);
        drawChannelMatrix(painter, page, channel,
                          QStringLiteral("Покрытие %1/%2: аттенюатор x фаза")
                              .arg(channel.completed_states).arg(channel.expected_states),
                          QStringLiteral("Статус"),
                          [](const ChannelStateReport&) { return 0.0; }, true, false);

        writer.newPage();
        page = writer.pageLayout().paintRectPixels(writer.resolution());
        drawChannelHeader(painter, page, channelHeader, info.demo_mode, 3, pageCount);
        drawChannelMatrix(painter, page, channel,
                          QStringLiteral("Измеренное ослабление на %1 ГГц")
                              .arg(static_cast<double>(channel.center_frequency_hz) / 1e9,
                                   0, 'f', 6),
                          QStringLiteral("Ослабление, дБ"),
                          [](const ChannelStateReport& state) { return state.center_atten_db; },
                          false, false);

        writer.newPage();
        page = writer.pageLayout().paintRectPixels(writer.resolution());
        drawChannelHeader(painter, page, channelHeader, info.demo_mode, 4, pageCount);
        drawChannelMatrix(painter, page, channel,
                          QStringLiteral("Фазовая ошибка на %1 ГГц")
                              .arg(static_cast<double>(channel.center_frequency_hz) / 1e9,
                                   0, 'f', 6),
                          QStringLiteral("Ошибка, град."),
                          [](const ChannelStateReport& state) {
                              return state.center_phase_error_deg;
                          }, false, true);

        writer.newPage();
        page = writer.pageLayout().paintRectPixels(writer.resolution());
        painter.setPen(QColor(28, 57, 91));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 11, QFont::Bold));
        painter.drawText(QRectF(page.left() + 28, page.top() + 82, page.width() - 56, 24),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Не более 20 состояний: сначала invalid, затем по ошибке"));
        const qreal left = page.left() + 28;
        const qreal top = page.top() + 118;
        const qreal width = page.width() - 56;
        const qreal rowHeight = 25;
        const std::array<qreal, 8> x{
            left, left + width * 0.08, left + width * 0.20, left + width * 0.32,
            left + width * 0.50, left + width * 0.68, left + width * 0.86,
            left + width
        };
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(225, 235, 247));
        painter.drawRect(QRectF(left, top, width, rowHeight));
        painter.setPen(QColor(39, 52, 69));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::Bold));
        const std::array<QString, 7> headers{
            QStringLiteral("Канал"), QStringLiteral("Att"), QStringLiteral("Phase"),
            QStringLiteral("Частота, ГГц"), QStringLiteral("Ошибка att, дБ"),
            QStringLiteral("Ошибка фазы, °"), QStringLiteral("Статус")};
        for (int column = 0; column < 7; ++column) {
            painter.drawText(QRectF(x[static_cast<std::size_t>(column)], top,
                                    x[static_cast<std::size_t>(column + 1)]
                                        - x[static_cast<std::size_t>(column)], rowHeight),
                             Qt::AlignCenter, headers[static_cast<std::size_t>(column)]);
        }
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 8));
        for (std::size_t row = 0; row < channel.worst_states.size(); ++row) {
            const auto& worst = channel.worst_states[row];
            const qreal rowTop = top + rowHeight * static_cast<qreal>(row + 1);
            if (row % 2 == 1) {
                painter.fillRect(QRectF(left, rowTop, width, rowHeight),
                                 QColor(248, 250, 253));
            }
            const std::array<QString, 7> cells{
                QString::number(channel.channel), QString::number(worst.att_code),
                QString::number(worst.phase_code),
                QString::number(static_cast<double>(worst.frequency_hz) / 1e9, 'f', 6),
                QString::number(worst.atten_error_db, 'f', 4),
                QString::number(worst.phase_error_deg, 'f', 4),
                worst.valid ? QStringLiteral("valid") : QStringLiteral("error")};
            painter.setPen(worst.valid ? QColor(45, 55, 68) : QColor(165, 42, 42));
            for (int column = 0; column < 7; ++column) {
                painter.drawText(QRectF(x[static_cast<std::size_t>(column)], rowTop,
                                        x[static_cast<std::size_t>(column + 1)]
                                            - x[static_cast<std::size_t>(column)], rowHeight),
                                 Qt::AlignCenter, cells[static_cast<std::size_t>(column)]);
            }
        }
        drawChannelHeader(painter, page, channelHeader, info.demo_mode, 5, pageCount);
        painter.end();
    }
    if (!file.commit()) {
        diagnostics = "channel PDF commit failed";
        return false;
    }
    return true;
}

}  // namespace

bool channelReportPasses(const ChannelReportInfo& channel) noexcept
{
    return channel.expected_states > 0
        && channel.completed_states == channel.expected_states
        && channel.valid_states == channel.expected_states
        && channel.error_states == 0;
}

bool loadThruApprovalJson(const std::filesystem::path& path, RunReportInfo& info)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    try {
        nlohmann::json j;
        in >> j;
        if (j.contains("thru_limit_mag_db") && j["thru_limit_mag_db"].is_number()) {
            info.thru_limit_mag_db = j["thru_limit_mag_db"].get<double>();
        }
        if (j.contains("thru_limit_phase_deg") && j["thru_limit_phase_deg"].is_number()) {
            info.thru_limit_phase_deg = j["thru_limit_phase_deg"].get<double>();
        }
        if (j.contains("thru_measured_mag_db") && j["thru_measured_mag_db"].is_number()) {
            info.thru_measured_mag_db = j["thru_measured_mag_db"].get<double>();
        }
        if (j.contains("thru_measured_phase_deg") && j["thru_measured_phase_deg"].is_number()) {
            info.thru_measured_phase_deg = j["thru_measured_phase_deg"].get<double>();
        }
        if (j.contains("metrologist_approved") && j["metrologist_approved"].is_boolean()) {
            info.metrologist_approved = j["metrologist_approved"].get<bool>();
        }
        if (j.contains("metrologist_name") && j["metrologist_name"].is_string()) {
            info.metrologist_name = j["metrologist_name"].get<std::string>();
        }
        if (j.contains("metrologist_date") && j["metrologist_date"].is_string()) {
            info.metrologist_date = j["metrologist_date"].get<std::string>();
        }
        info.thru_meta_loaded = true;
        return true;
    } catch (...) {
        return false;
    }
}

bool writeRunReportPdf(const std::filesystem::path& path,
                       const RunReportInfo& info,
                       std::string& diagnostics)
{
    if (info.detailed_channel_report && info.channels.size() == 1
        && !info.channels.front().states.empty()) {
        return writeDetailedChannelReportPdf(path, info, diagnostics);
    }
    QSaveFile file(QString::fromStdWString(path.wstring()));
    if (!file.open(QIODevice::WriteOnly)) {
        diagnostics = "cannot write pdf: " + path.string();
        return false;
    }
    {
        QPdfWriter writer(&file);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageOrientation(QPageLayout::Portrait);
        writer.setResolution(96);
        const bool singleChannel = info.channels.size() == 1;
        const QString reportTitle = singleChannel
            ? QStringLiteral("Отчёт калибровки канала %1")
                  .arg(info.channels.front().channel)
            : QStringLiteral("Отчёт калибровки приёмных каналов");
        writer.setTitle(QStringLiteral("%1 - %2").arg(reportTitle, qString(info.run_id)));
        writer.setCreator(QStringLiteral("AFAR RX Calibration Studio"));

        QPainter painter(&writer);
        if (!painter.isActive()) {
            diagnostics = "cannot start PDF painter";
            return false;
        }
        const QRectF page = writer.pageLayout().paintRectPixels(writer.resolution());
        qreal y = page.top() + 22;
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 16, QFont::Bold));
        painter.drawText(page.adjusted(28, 0, -28, 0), Qt::AlignHCenter | Qt::AlignTop,
                         reportTitle);
        y += 42;
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 10));
        drawWrappedLine(painter, page, y, QStringLiteral("Серия: %1").arg(qString(info.run_id)));
        drawWrappedLine(painter, page, y,
                        QStringLiteral("Каталог: %1").arg(qString(info.series_path)));
        drawWrappedLine(painter, page, y,
                        QStringLiteral("VNA: %1").arg(qString(info.vna_idn)));
        if (info.demo_mode) {
            painter.setPen(QColor(160, 55, 35));
            drawWrappedLine(
                painter, page, y,
                QStringLiteral("Источник: S2VNA DEMO — имитация, не метрология."));
            painter.setPen(Qt::black);
        }
        drawWrappedLine(
            painter, page, y,
            QStringLiteral("Диапазон: %1 - %2 ГГц; точек: %3")
                .arg(static_cast<double>(info.f_start_hz) / 1e9, 0, 'f', 6)
                .arg(static_cast<double>(info.f_stop_hz) / 1e9, 0, 'f', 6)
                .arg(info.frequency_points));
        drawWrappedLine(
            painter, page, y,
            QStringLiteral("Сетка: каналов %1; кодов аттенюации %2; кодов фазы %3")
                .arg(info.channels.size()).arg(info.attenuator_codes).arg(info.phase_codes));
        drawWrappedLine(
            painter, page, y,
            QStringLiteral("Завершено состояний: %1; valid direct: %2; valid inverse: %3")
                .arg(info.completed_states).arg(info.valid_direct_count)
                .arg(info.valid_inverse_count));

        y += 8;
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 11, QFont::Bold));
        drawWrappedLine(painter, page, y, QStringLiteral("Калибровка и контроль качества"));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 10));
        drawWrappedLine(painter, page, y,
                        QStringLiteral("THRU: %1").arg(makeThruSummary(info)));
        drawWrappedLine(
            painter, page, y,
            QStringLiteral("Пределы ПО: дрейф фазы %1°; остаток фазы %2°")
                .arg(info.max_drift_phase_deg, 0, 'g', 6)
                .arg(info.max_phase_residual_deg, 0, 'g', 6));
        const QString metroName = info.metrologist_name.empty()
            ? QStringLiteral("-") : qString(info.metrologist_name);
        const QString metroDate = info.metrologist_date.empty()
            ? QStringLiteral("-") : qString(info.metrologist_date);
        drawWrappedLine(
            painter, page, y,
            QStringLiteral("Метролог: %1; дата: %2; подтверждение: %3")
                .arg(metroName, metroDate,
                     info.metrologist_approved ? QStringLiteral("да") : QStringLiteral("нет")));
        painter.setPen(QColor(120, 55, 35));
        drawWrappedLine(painter, page, y,
                        QStringLiteral("Пределы являются настройками ПО и не заменяют "
                                       "метрологическую аттестацию."));
        painter.setPen(Qt::black);

        y += 8;
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 11, QFont::Bold));
        drawWrappedLine(painter, page, y, QStringLiteral("Версия и воспроизводимость"));
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 9));
        drawWrappedLine(
            painter, page, y,
            QStringLiteral("ПО %1; commit %2; %3 %4; C++%5; Qt %6; nlohmann_json %7")
                .arg(softwareVersion(info), buildCommit(), QStringLiteral(AFAR_CXX_COMPILER_ID),
                     QStringLiteral(AFAR_CXX_COMPILER_VERSION), QStringLiteral(AFAR_CXX_STANDARD),
                     QStringLiteral(AFAR_QT_VERSION), QStringLiteral(AFAR_NLOHMANN_JSON_VERSION)));
        drawFooter(painter, page, 1);

        writer.newPage();
        const QRectF channelPage = writer.pageLayout().paintRectPixels(writer.resolution());
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 15, QFont::Bold));
        painter.drawText(channelPage.adjusted(28, 18, -28, 0),
                         Qt::AlignHCenter | Qt::AlignTop,
                         singleChannel
                             ? QStringLiteral("Результат канала %1")
                                   .arg(info.channels.front().channel)
                             : QStringLiteral("Результаты по каналам"));

        const qreal left = channelPage.left() + 28;
        const qreal top = channelPage.top() + 62;
        const qreal rowHeight = 27;
        const std::array<qreal, 6> x{left, left + 70, left + 190, left + 320,
                                     left + 450, channelPage.right() - 28};
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 8, QFont::Bold));
        painter.setBrush(QColor(225, 235, 247));
        painter.drawRect(QRectF(x[0], top, x[5] - x[0], rowHeight));
        const std::array<QString, 5> headers{QStringLiteral("Канал"),
                                             QStringLiteral("Состояния"),
                                             QStringLiteral("Direct valid"),
                                             QStringLiteral("Inverse valid"),
                                             QStringLiteral("Статус")};
        for (int column = 0; column < 5; ++column) {
            painter.drawText(QRectF(x[static_cast<std::size_t>(column)], top,
                                    x[static_cast<std::size_t>(column + 1)]
                                        - x[static_cast<std::size_t>(column)], rowHeight),
                             Qt::AlignCenter, headers[static_cast<std::size_t>(column)]);
        }
        painter.setFont(QFont(QStringLiteral("Sans Serif"), 9));
        painter.setBrush(Qt::NoBrush);
        for (std::size_t row = 0; row < info.channels.size(); ++row) {
            const auto& channel = info.channels[row];
            const qreal rowTop = top + rowHeight * static_cast<qreal>(row + 1);
            const bool ok = channel.completed_states > 0 && channel.valid_direct_count > 0
                && channel.valid_inverse_count > 0;
            const std::array<QString, 5> cells{
                QString::number(channel.channel), QString::number(channel.completed_states),
                QString::number(channel.valid_direct_count),
                QString::number(channel.valid_inverse_count),
                ok ? QStringLiteral("OK") : QStringLiteral("Проверить")};
            for (int column = 0; column < 5; ++column) {
                painter.drawText(QRectF(x[static_cast<std::size_t>(column)], rowTop,
                                        x[static_cast<std::size_t>(column + 1)]
                                            - x[static_cast<std::size_t>(column)], rowHeight),
                                 Qt::AlignCenter, cells[static_cast<std::size_t>(column)]);
            }
            painter.setPen(QColor(180, 180, 180));
            painter.drawLine(QPointF(x[0], rowTop + rowHeight),
                             QPointF(x[5], rowTop + rowHeight));
            painter.setPen(Qt::black);
        }
        painter.setPen(QColor(120, 120, 120));
        for (const qreal columnX : x) {
            painter.drawLine(QPointF(columnX, top),
                             QPointF(columnX, top + rowHeight * (info.channels.size() + 1)));
        }
        painter.setPen(Qt::black);
        drawFooter(painter, channelPage, 2);
        painter.end();
    }
    if (!file.commit()) {
        diagnostics = "PDF commit failed";
        return false;
    }
    return true;
}

bool isValidPdfSmoke(const std::filesystem::path& path, std::string& diagnostics)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot open pdf";
        return false;
    }
    char head[5]{};
    in.read(head, 5);
    if (!in || std::string(head, 5) != "%PDF-") {
        diagnostics = "not a PDF header";
        return false;
    }
    in.seekg(0, std::ios::end);
    if (in.tellg() <= 0) {
        diagnostics = "empty pdf";
        return false;
    }
    return true;
}

}  // namespace afar::report
