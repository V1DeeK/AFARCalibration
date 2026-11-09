#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <array>

/// Один шаг прохода канала: аттенюация, фаза или оба кода вместе.
struct ChannelStep {
    int channel = 0;  ///< 0…3
    int stage = 0;    ///< 0 аттенюация, 1 фаза, 2 оба вместе
    int phase = 0;
    int atten = 0;
};

struct ChannelPoint {
    ChannelStep step;
    double phaseDeg = 0;
    double attenDb = 0;
    double s11Db = 0;
    double s21Db = 0;
    double s12Db = 0;
    double s22Db = 0;
    QString imageFile;
    QVector<double> freqGhz;
    QVector<double> s11mag;
    QVector<double> s21mag;
    QVector<double> s12mag;
    QVector<double> s22mag;
};

/// Три прохода по кодам 0…max: аттенюация при сохранённой фазе, фаза при
/// сохранённой аттенюации, затем оба кода одним шагом.
[[nodiscard]] QVector<ChannelStep> buildChannelSteps(const QVector<int>& channels, int phaseMax, int attenMax,
                                                     const std::array<int, 4>& holdPhase,
                                                     const std::array<int, 4>& holdAtten);

[[nodiscard]] QString channelStageName(int stage);

/// |S| на средней частоте. Пустой вектор — тихое значение.
[[nodiscard]] double channelMidDb(const QVector<double>& magDb);

[[nodiscard]] QImage renderChannelGraph(const ChannelPoint& point, const QSize& size = QSize(900, 360));

/// Пишет report.pdf и кладёт снимки графиков в подкаталог graphs. Пустая строка — успех.
[[nodiscard]] QString writeChannelReport(const QString& folder, const QString& title, const QVector<ChannelPoint>& points);

struct RepeatStats {
    int count = 0;
    double mean = 0;
    double stdev = 0;
};

/// Среднее и выборочное СКО. Пустой набор — нули. Один отсчёт — СКО 0.
[[nodiscard]] RepeatStats repeatStats(const QVector<double>& values);

/// PDF повторяемости: каждое значение получено после повторной установки того же кода.
[[nodiscard]] QString writeRepeatReport(const QString& folder, const QString& title, int channel,
                                       double phaseDeg, double attenDb, const QString& parameter,
                                       const QVector<double>& valuesDb);
