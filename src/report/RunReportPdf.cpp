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

#include <fstream>
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

}  // namespace

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
