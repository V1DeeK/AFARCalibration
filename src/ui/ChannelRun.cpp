#include "ChannelRun.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPen>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextLayout>

#include <algorithm>
#include <cmath>
#include <numeric>

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

void drawPdfText(QPainter& painter, const QFont& font, const QRectF& rect, const QString& text,
                 const QColor& color, const QString& href, const QString& anchor)
{
    QTextCharFormat format;
    format.setFont(font);
    format.setForeground(color);
    if (!href.isEmpty() || !anchor.isEmpty()) {
        format.setAnchor(true);
    }
    if (!href.isEmpty()) {
        format.setAnchorHref(href);
        format.setFontUnderline(true);
    }
    if (!anchor.isEmpty()) {
        format.setAnchorNames({anchor});
    }
    QTextLayout layout(text, font);
    layout.setFormats({QTextLayout::FormatRange{0, static_cast<int>(text.size()), format}});
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(rect.width());
    }
    layout.endLayout();
    const qreal top = rect.top() + std::max<qreal>(0, (rect.height() - layout.boundingRect().height()) / 2.0);
    layout.draw(&painter, QPointF(rect.left(), top));
}

}  // namespace

QVector<ChannelStep> buildChannelSteps(const QVector<int>& channels, int phaseMax, int attenMax,
                                       const std::array<int, 4>& holdPhase, const std::array<int, 4>& holdAtten)
{
    Q_UNUSED(holdPhase);
    Q_UNUSED(holdAtten);
    QVector<ChannelStep> steps;
    const int phaseLast = std::max(0, phaseMax);
    const int attenLast = std::max(0, attenMax);
    for (const int channel : channels) {
        if (channel < 0 || channel > 3) {
            continue;
        }
        for (int phase = 0; phase <= phaseLast; ++phase) {
            for (int atten = 0; atten <= attenLast; ++atten) {
                steps.append(ChannelStep{channel, 0, phase, atten});
            }
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

double vswrFromReturnDb(double s11Db)
{
    if (!std::isfinite(s11Db)) {
        return 0;
    }
    const double magnitude = std::pow(10.0, s11Db / 20.0);
    if (magnitude >= 0.999) {
        return 999.0;
    }
    if (magnitude <= 0.0) {
        return 1.0;
    }
    return (1.0 + magnitude) / (1.0 - magnitude);
}

void deriveChannelPoint(ChannelPoint& point)
{
    point.s11Db = channelMidDb(point.s11mag);
    point.s21Db = channelMidDb(point.s21mag);
    point.s12Db = channelMidDb(point.s12mag);
    point.s22Db = channelMidDb(point.s22mag);
    point.peakDb = -120.0;
    int peakAt = -1;
    for (int i = 0; i < point.s21mag.size(); ++i) {
        const double value = point.s21mag.at(i);
        if (std::isfinite(value) && value > point.peakDb) {
            point.peakDb = value;
            peakAt = i;
        }
    }
    point.kuDb = point.s21mag.isEmpty() ? -120.0 : point.s21Db;
    double reflection = point.s11Db;
    if (peakAt >= 0 && peakAt < point.s11mag.size() && std::isfinite(point.s11mag.at(peakAt))) {
        reflection = point.s11mag.at(peakAt);
    }
    point.vswr = point.s11mag.isEmpty() && point.s21mag.isEmpty() ? 0.0 : vswrFromReturnDb(reflection);
}

QVector<int> pickScreenshotSteps(int stepCount, int sampleCount, quint32 seed)
{
    if (stepCount <= 0 || sampleCount <= 0) {
        return {};
    }
    QVector<int> order(stepCount);
    std::iota(order.begin(), order.end(), 0);
    if (stepCount <= sampleCount) {
        return order;
    }
    QRandomGenerator random(seed);
    const int take = sampleCount;
    for (int i = 0; i < take; ++i) {
        const int swap = i + static_cast<int>(random.bounded(static_cast<quint32>(stepCount - i)));
        std::swap(order[i], order[swap]);
    }
    order.resize(take);
    std::sort(order.begin(), order.end());
    return order;
}

int channelSweepSettleMs(int points, int ifbwHz, int averages)
{
    if (points < 1) {
        points = 1;
    }
    if (ifbwHz < 1) {
        ifbwHz = 1;
    }
    if (averages < 1) {
        averages = 1;
    }
    const double oneSweepMs = (static_cast<double>(points) * static_cast<double>(averages) * 1000.0)
                              / static_cast<double>(ifbwHz);
    const int pause = static_cast<int>(std::ceil(oneSweepMs * 2.0)) + 50;
    return std::max(100, pause);
}

int bestPowerIndex(const QVector<ChannelPoint>& points, int channel)
{
    int best = -1;
    double peak = -1.0e9;
    for (int i = 0; i < points.size(); ++i) {
        if (channel >= 0 && points.at(i).step.channel != channel) {
            continue;
        }
        if (!points.at(i).peakValid || !std::isfinite(points.at(i).peakDb)) {
            continue;
        }
        if (points.at(i).peakDb > peak) {
            peak = points.at(i).peakDb;
            best = i;
        }
    }
    return best;
}

QString productChannelConfigPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                        + QStringLiteral("/AFAR RX Calibration");
    QDir().mkpath(dir);
    return dir + QStringLiteral("/product-channels.json");
}

namespace {

QJsonObject rowObject(const ProductChannelSetting& row)
{
    return QJsonObject{
        {QStringLiteral("channel"), row.channel},
        {QStringLiteral("phaseCode"), row.phaseCode},
        {QStringLiteral("attenCode"), row.attenCode},
        {QStringLiteral("phaseDeg"), row.phaseDeg},
        {QStringLiteral("attenDb"), row.attenDb},
        {QStringLiteral("peakDb"), row.peakDb},
        {QStringLiteral("kuDb"), row.kuDb},
        {QStringLiteral("vswr"), row.vswr},
        {QStringLiteral("saved"), row.saved},
    };
}

ProductChannelSetting rowFromObject(const QJsonObject& object, int product, int channel)
{
    ProductChannelSetting row;
    row.product = product;
    row.channel = channel;
    row.phaseCode = object.value(QStringLiteral("phaseCode")).toInt(0);
    row.attenCode = object.value(QStringLiteral("attenCode")).toInt(0);
    row.phaseDeg = object.value(QStringLiteral("phaseDeg")).toDouble(0);
    row.attenDb = object.value(QStringLiteral("attenDb")).toDouble(-31.5);
    row.peakDb = object.value(QStringLiteral("peakDb")).toDouble(-120);
    row.kuDb = object.value(QStringLiteral("kuDb")).toDouble(-120);
    row.vswr = object.value(QStringLiteral("vswr")).toDouble(0);
    row.saved = object.value(QStringLiteral("saved")).toBool(false);
    return row;
}

}  // namespace

bool saveProductChannel(const ProductChannelSetting& row, QString* error)
{
    if (row.product < 1 || row.product > 4 || row.channel < 1 || row.channel > 4) {
        if (error != nullptr) {
            *error = QStringLiteral("изделие и канал должны быть от 1 до 4");
        }
        return false;
    }
    const QString path = productChannelConfigPath();
    QJsonArray products;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(existing.readAll());
        products = doc.object().value(QStringLiteral("products")).toArray();
        existing.close();
    }
    bool foundProduct = false;
    for (int i = 0; i < products.size(); ++i) {
        QJsonObject product = products.at(i).toObject();
        if (product.value(QStringLiteral("product")).toInt() != row.product) {
            continue;
        }
        foundProduct = true;
        QJsonArray channels = product.value(QStringLiteral("channels")).toArray();
        bool foundChannel = false;
        const QJsonObject stored = rowObject(row);
        for (int c = 0; c < channels.size(); ++c) {
            if (channels.at(c).toObject().value(QStringLiteral("channel")).toInt() == row.channel) {
                channels.replace(c, stored);
                foundChannel = true;
                break;
            }
        }
        if (!foundChannel) {
            channels.append(stored);
        }
        product.insert(QStringLiteral("channels"), channels);
        products.replace(i, product);
        break;
    }
    if (!foundProduct) {
        products.append(QJsonObject{{QStringLiteral("product"), row.product},
                                    {QStringLiteral("channels"), QJsonArray{rowObject(row)}}});
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error != nullptr) {
            *error = QStringLiteral("не удалось записать ") + path;
        }
        return false;
    }
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("products"), products}}).toJson(QJsonDocument::Indented));
    return true;
}

std::array<ProductChannelSetting, 4> loadProductChannels(int product)
{
    std::array<ProductChannelSetting, 4> rows{};
    for (int i = 0; i < 4; ++i) {
        rows[static_cast<std::size_t>(i)].product = product;
        rows[static_cast<std::size_t>(i)].channel = i + 1;
    }
    QFile file(productChannelConfigPath());
    if (!file.open(QIODevice::ReadOnly)) {
        return rows;
    }
    const QJsonArray products = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("products")).toArray();
    for (const QJsonValue& value : products) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("product")).toInt() != product) {
            continue;
        }
        for (const QJsonValue& channelValue : object.value(QStringLiteral("channels")).toArray()) {
            const QJsonObject channel = channelValue.toObject();
            const int index = channel.value(QStringLiteral("channel")).toInt() - 1;
            if (index < 0 || index > 3) {
                continue;
            }
            rows[static_cast<std::size_t>(index)] = rowFromObject(channel, product, index + 1);
        }
        break;
    }
    return rows;
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

QString writeChannelCsv(const QString& folder, const QVector<ChannelPoint>& points)
{
    QFile file(folder + QStringLiteral("/measurements.csv"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return QStringLiteral("не удалось записать CSV");
    }
    file.write("channel_id,freq_hz,phase_code,att_code,gain_db,phase_deg\n");
    for (const ChannelPoint& point : points) {
        const int count = std::min(point.s21FreqHz.size(),
                                    std::min(point.s21GainDb.size(), point.s21PhaseDeg.size()));
        for (int i = 0; i < count; ++i) {
            const double frequency = point.s21FreqHz.at(i);
            const double gain = point.s21GainDb.at(i);
            const double phase = point.s21PhaseDeg.at(i);
            if (!std::isfinite(frequency) || frequency <= 0.0 || !std::isfinite(gain) || !std::isfinite(phase)) {
                continue;
            }
            const QString line = QStringLiteral("%1,%2,%3,%4,%5,%6\n")
                                     .arg(point.step.channel)
                                     .arg(static_cast<qint64>(std::llround(frequency)))
                                     .arg(point.step.phase)
                                     .arg(point.step.atten)
                                     .arg(gain, 0, 'f', 2)
                                     .arg(phase, 0, 'f', 2);
            file.write(line.toUtf8());
        }
    }
    return QString();
}

QString writeChannelReport(const QString& folder, const QString& title, const QVector<ChannelPoint>& points,
                           const QString& note)
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
    if (!note.isEmpty()) {
        label(28, 76, note, 10);
    }
    const int best = bestPowerIndex(points);
    if (best >= 0) {
        const ChannelPoint& peak = points.at(best);
        font.setPixelSize(12);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#9a3412")));
        painter.drawText(QPointF(28, note.isEmpty() ? 78 : 96),
                         QStringLiteral("Максимум по пику S21: изделие %1, канал %2, фаза %3°, аттенюация %4 дБ, пик %5 дБ")
                             .arg(peak.product)
                             .arg(peak.step.channel + 1)
                             .arg(peak.phaseDeg, 0, 'f', 1)
                             .arg(peak.attenDb, 0, 'f', 1)
                             .arg(peak.peakDb, 0, 'f', 1));
    }
    label(28, note.isEmpty() ? 98 : 116,
          QStringLiteral("Точек: %1. S11, S22, S21 и S12 — максимум маркера своего графика. "
                         "S11 и S22 в формате КСВ записаны как на анализаторе. Пик — маркер пика на S21, дБ. "
                         "Жёлтая строка — наибольший пик. «график» открывает снимок этой точки.")
              .arg(points.size()),
          10);

    const QStringList headers{QStringLiteral("Канал"), QStringLiteral("Фаза"), QStringLiteral("Аттен."),
                              QStringLiteral("S11"), QStringLiteral("S22"), QStringLiteral("S21"),
                              QStringLiteral("S12"), QStringLiteral("Пик"), QStringLiteral("Снимок")};
    const qreal tableLeft = 24;
    const qreal tableW = w - 48;
    const qreal colW = tableW / headers.size();
    const qreal rowH = 20;
    qreal y = note.isEmpty() ? 132 : 150;
    const auto paintRow = [&](const QStringList& cells, bool highlight, bool header, const QString& shotAnchor) {
        if (highlight) {
            painter.fillRect(QRectF(tableLeft, y, tableW, rowH), QColor(QStringLiteral("#ffe600")));
        } else if (header) {
            painter.fillRect(QRectF(tableLeft, y, tableW, rowH), QColor(QStringLiteral("#e7f5f1")));
        }
        painter.setPen(QPen(QColor(QStringLiteral("#334155")), 1));
        painter.drawRect(QRectF(tableLeft, y, tableW, rowH));
        for (int c = 1; c < cells.size(); ++c) {
            const qreal x = tableLeft + c * colW;
            painter.drawLine(QPointF(x, y), QPointF(x, y + rowH));
        }
        font.setPixelSize(header ? 10 : 9);
        font.setBold(header || highlight);
        painter.setFont(font);
        painter.setPen(highlight ? QColor(QStringLiteral("#7c2d12")) : QColor(QStringLiteral("#193448")));
        for (int c = 0; c < cells.size(); ++c) {
            if (!shotAnchor.isEmpty() && c + 1 == cells.size()) {
                QFont linkFont = font;
                linkFont.setBold(false);
                linkFont.setPixelSize(9);
                drawPdfText(painter, linkFont, QRectF(tableLeft + c * colW + 4, y, colW - 8, rowH),
                            cells.at(c), QColor(QStringLiteral("#1d4ed8")),
                            QStringLiteral("#") + shotAnchor, QString());
                continue;
            }
            painter.drawText(QRectF(tableLeft + c * colW + 4, y, colW - 8, rowH),
                             Qt::AlignVCenter | Qt::AlignLeft, cells.at(c));
        }
        font.setBold(false);
        y += rowH;
    };
    const auto headerRow = [&] { paintRow(headers, false, true, QString()); };
    headerRow();
    for (int index = 0; index < points.size(); ++index) {
        const ChannelPoint& point = points.at(index);
        if (y + rowH > h - 28) {
            footer();
            pdf.newPage();
            ++page;
            y = 36;
            headerRow();
        }
        const auto markerText = [](bool ok, double value, bool swr) {
            if (!ok || !std::isfinite(value)) {
                return QStringLiteral("—");
            }
            return QString::number(value, 'f', swr ? 3 : 1);
        };
        const bool hasShot = !point.imageFile.isEmpty() && QFile::exists(point.imageFile);
        const QString shotAnchor = hasShot ? QStringLiteral("g%1").arg(index) : QString();
        const QStringList cells{QString::number(point.step.channel + 1),
                                QStringLiteral("%1°").arg(point.phaseDeg, 0, 'f', 1),
                                QStringLiteral("%1 дБ").arg(point.attenDb, 0, 'f', 1),
                                markerText(point.s11Ok, point.s11Db, point.s11Swr),
                                markerText(point.s22Ok, point.s22Db, point.s22Swr),
                                markerText(point.s21Ok, point.s21Db, false),
                                markerText(point.s12Ok, point.s12Db, false),
                                point.peakValid ? QString::number(point.peakDb, 'f', 1) : QStringLiteral("—"),
                                hasShot ? QStringLiteral("график") : QStringLiteral("—")};
        paintRow(cells, index == best, false, shotAnchor);
    }
    footer();

    for (int index = 0; index < points.size(); ++index) {
        const ChannelPoint& point = points.at(index);
        if (point.imageFile.isEmpty() || !QFile::exists(point.imageFile)) {
            continue;
        }
        const QImage image(point.imageFile);
        if (image.isNull()) {
            continue;
        }
        pdf.newPage();
        ++page;
        font.setPixelSize(12);
        font.setBold(false);
        const QString caption = QStringLiteral("Изделие %1 канал %2 аттенюация %3 дБ фаза %4°")
                                    .arg(point.product)
                                    .arg(point.step.channel + 1)
                                    .arg(point.attenDb, 0, 'f', 1)
                                    .arg(point.phaseDeg, 0, 'f', 1);
        drawPdfText(painter, font, QRectF(28, 12, w - 56, 22), caption, QColor(QStringLiteral("#193448")),
                    QString(), QStringLiteral("g%1").arg(index));
        const QRect target(28, 40, static_cast<int>(w - 56), static_cast<int>(h - 70));
        painter.drawImage(target, image);
        footer();
    }
    painter.end();
    return QString();
}

RepeatStats repeatStats(const QVector<double>& values)
{
    RepeatStats stats;
    stats.count = static_cast<int>(values.size());
    if (values.isEmpty()) {
        return stats;
    }
    double sum = 0;
    for (const double value : values) {
        sum += value;
    }
    stats.mean = sum / static_cast<double>(values.size());
    if (values.size() < 2) {
        return stats;
    }
    double acc = 0;
    for (const double value : values) {
        const double delta = value - stats.mean;
        acc += delta * delta;
    }
    stats.stdev = std::sqrt(acc / static_cast<double>(values.size() - 1));
    return stats;
}

QString writeRepeatReport(const QString& folder, const QString& title, int channel, double phaseDeg,
                          double attenDb, const QString& parameter, const QVector<double>& valuesDb,
                          int rejected)
{
    const QString path = folder + QStringLiteral("/repeat.pdf");
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
    const RepeatStats stats = repeatStats(valuesDb);
    const auto label = [&](qreal x, qreal y, const QString& text, int size) {
        font.setPixelSize(size);
        painter.setFont(font);
        painter.setPen(QColor(QStringLiteral("#193448")));
        painter.drawText(QPointF(x, y), text);
    };
    label(28, 36, QStringLiteral("Повторяемость S-параметра"), 18);
    label(28, 58, title, 11);
    label(28, 80, QStringLiteral("Канал %1 · фаза %2° · аттенюация %3 дБ · %4")
                      .arg(channel + 1)
                      .arg(phaseDeg, 0, 'f', 3)
                      .arg(attenDb, 0, 'f', 2)
                      .arg(parameter),
          12);
    label(28, 104, QStringLiteral("Подтверждённых установок %1, кадр не совпал %2. "
                                 "Мат. ожидание %3 дБ, СКО %4 дБ.")
                       .arg(stats.count)
                       .arg(rejected)
                       .arg(stats.mean, 0, 'f', 3)
                       .arg(stats.stdev, 0, 'f', 3),
          12);
    qreal y = 132;
    font.setPixelSize(10);
    painter.setFont(font);
    for (int i = 0; i < valuesDb.size(); ++i) {
        if (y > painter.viewport().height() - 28) {
            pdf.newPage();
            y = 36;
        }
        painter.drawText(QPointF(28, y), QStringLiteral("%1. %2 дБ").arg(i + 1).arg(valuesDb.at(i), 0, 'f', 3));
        y += 14;
    }
    Q_UNUSED(w);
    painter.end();
    return QString();
}
