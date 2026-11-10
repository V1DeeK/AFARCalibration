#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <array>

/// Один шаг сетки: при фиксированной фазе — очередной код аттенюации.
struct ChannelStep {
    int channel = 0;  ///< 0…3
    int stage = 0;
    int phase = 0;
    int atten = 0;
};

struct ChannelPoint {
    ChannelStep step;
    int product = 1;
    double phaseDeg = 0;
    double attenDb = 0;
    /// Максимум маркера своего графика. У S11/S22 это показанное значение (КСВ, если формат SWR).
    double s11Db = 0;
    double s21Db = 0;
    double s12Db = 0;
    double s22Db = 0;
    bool s11Ok = false;
    bool s21Ok = false;
    bool s12Ok = false;
    bool s22Ok = false;
    bool s11Swr = false;
    bool s22Swr = false;
    /// Маркер пика на графике S21, дБ.
    double peakDb = -120;
    bool peakValid = false;
    double kuDb = -120;
    double vswr = 0;
    QString imageFile;
    /// Комплексный S21 по частотам свипа: Гц, усиление дБ, фаза градусы. Фаза не нормируется.
    QVector<double> s21FreqHz;
    QVector<double> s21GainDb;
    QVector<double> s21PhaseDeg;
    QVector<double> freqGhz;
    QVector<double> s11mag;
    QVector<double> s21mag;
    QVector<double> s12mag;
    QVector<double> s22mag;
};

/// Сетка выбранных каналов: для каждой фазы 0…phaseMax все аттенюации 0…attenMax.
/// holdPhase и holdAtten не задают шаг: проход больше не держит один код.
[[nodiscard]] QVector<ChannelStep> buildChannelSteps(const QVector<int>& channels, int phaseMax, int attenMax,
                                                     const std::array<int, 4>& holdPhase,
                                                     const std::array<int, 4>& holdAtten);

[[nodiscard]] QString channelStageName(int stage);

/// |S| на средней частоте. Пустой вектор — тихое значение.
[[nodiscard]] double channelMidDb(const QVector<double>& magDb);

/// Пик S21, КУ (S21 на середине свипа) и КСВ из S11 на частоте этого пика.
void deriveChannelPoint(ChannelPoint& point);

/// Индекс точки с наибольшим пиком маркера S21. Нет ни одного пика — −1.
/// channel < 0 — среди всех точек, иначе только у этого канала (0…3).
[[nodiscard]] int bestPowerIndex(const QVector<ChannelPoint>& points, int channel = -1);

/// Случайные номера шагов для снимков. Если шагов не больше sampleCount — все, по возрастанию.
[[nodiscard]] QVector<int> pickScreenshotSteps(int stepCount, int sampleCount, quint32 seed);

/// Пауза после кода, мс: два свипа (точки × усреднение / полоса ПЧ) плюс 50 мс, не короче 100 мс.
[[nodiscard]] int channelSweepSettleMs(int points, int ifbwHz, int averages);

struct ProductChannelSetting {
    int product = 1;
    int channel = 1;
    int phaseCode = 0;
    int attenCode = 0;
    double phaseDeg = 0;
    double attenDb = -31.5;
    double peakDb = -120;
    double kuDb = -120;
    double vswr = 0;
    bool saved = false;
};

/// Документы\AFAR RX Calibration\product-channels.json — 4 изделия по 4 канала.
[[nodiscard]] QString productChannelConfigPath();
[[nodiscard]] bool saveProductChannel(const ProductChannelSetting& row, QString* error);
[[nodiscard]] std::array<ProductChannelSetting, 4> loadProductChannels(int product);

[[nodiscard]] QImage renderChannelGraph(const ChannelPoint& point, const QSize& size = QSize(900, 360));

/// Пишет report.pdf и кладёт снимки графиков в подкаталог graphs. Пустая строка — успех.
[[nodiscard]] QString writeChannelReport(const QString& folder, const QString& title,
                                         const QVector<ChannelPoint>& points, const QString& note = {});

/// measurements.csv рядом с PDF. Пустая строка — успех.
[[nodiscard]] QString writeChannelCsv(const QString& folder, const QVector<ChannelPoint>& points);

struct RepeatStats {
    int count = 0;
    double mean = 0;
    double stdev = 0;
};

/// Среднее и выборочное СКО. Пустой набор — нули. Один отсчёт — СКО 0.
[[nodiscard]] RepeatStats repeatStats(const QVector<double>& values);

/// PDF повторяемости: каждое значение получено после повторной установки того же кода.
/// rejected — установки, чей ответный HEX не совпал с отправленным кадром.
[[nodiscard]] QString writeRepeatReport(const QString& folder, const QString& title, int channel,
                                       double phaseDeg, double attenDb, const QString& parameter,
                                       const QVector<double>& valuesDb, int rejected = 0);
