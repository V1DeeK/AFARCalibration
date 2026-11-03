#pragma once

#include <QString>
#include <QVector>

struct HeterReportPoint {
    int n = 0;
    QString port;
    QString mode;
    double attA = 0;
    double attB = 0;
    double stepDb = 0;
    double setHz = 0;
    double peakHz = 0;
    double levelDbm = 0;
    bool signal = false;
    bool lock = false;
    double spanHz = 0;
    double rbwHz = 0;
    QString time;
    QString imageFile;
};

struct HeterReportRun {
    QString id;
    QString outcome;
    QString error;
    QString started;
    QString ended;
    QString instrument;
    QString analyzerIp;
    QString psuIp;
    QString routing;
    double attMin = 0;
    double attMax = 0;
    QString frequencies;
    double spanHz = 0;
    int total = 0;
    bool haveInitial = false;
    QString rs485Port;
    bool externalRef = false;
    int aDbm = 0;
    int bDbm = 0;
};

/// PDF протокола: титульная таблица и по странице на снимок FSW.
/// Возвращает пустую строку или текст ошибки.
QString writeHeterodyneReport(const QString& folder, const HeterReportRun& run, const QVector<HeterReportPoint>& points);
