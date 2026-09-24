#pragma once

#include <QVector>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class S21PlotWidget;

/// Вкладка «Измерение» (UI-02 / UI-004): этапы слева переключают центр.
class MeasureTab final : public QWidget {
    Q_OBJECT

public:
    explicit MeasureTab(QWidget* parent = nullptr);

    /// Частоты в герцах (внутреннее хранение / API).
    void applyRunConfigDefaults(double fStartHz,
                                double fStopHz,
                                int points,
                                int ifbwHz,
                                double powerDbm,
                                int averages);
    void setStageHighlight(int stageIndex0);
    void setProgress(qint64 completed, qint64 total, int channel, int attCode, int phaseCode);
    void setEtaText(const QString& text);
    /// Обратная совместимость: только трасса S21.
    void setSweepCurves(const QVector<double>& freqGhz,
                        const QVector<double>& magDb,
                        const QVector<double>& phaseUnwrapDeg);
    /// UI-302: mag + unwrap phase для S11/S21/S12/S22 (сетка 2×2).
    void setSparamsCurves(const QVector<double>& freqGhz,
                          const QVector<double>& s11mag,
                          const QVector<double>& s11ph,
                          const QVector<double>& s21mag,
                          const QVector<double>& s21ph,
                          const QVector<double>& s12mag,
                          const QVector<double>& s12ph,
                          const QVector<double>& s22mag,
                          const QVector<double>& s22ph);
    void setStandCheck(bool connectionsOk,
                       bool idnOk,
                       bool calOk,
                       const QString& thruText,
                       bool noOverload,
                       bool probeOk,
                       double powerDbm,
                       bool engineer);
    void setSeriesArtifacts(const QString& seriesRoot,
                            const QString& directLut,
                            const QString& inverseLut,
                            const QString& report,
                            const QString& manifest);
    /// UI-203/204: сводка с worker после Complete (фрагменты уже прочитаны вне UI-потока).
    void applySeriesArtifactsPreview(const QString& runId,
                                     qint64 completedStates,
                                     const QString& directPath,
                                     qint64 directValid,
                                     qint64 directTotal,
                                     bool directFlat,
                                     const QString& directFragment,
                                     const QString& inversePath,
                                     qint64 inverseValid,
                                     qint64 inverseTotal,
                                     bool inverseFlat,
                                     const QString& inverseFragment,
                                     const QString& reportPath,
                                     qint64 reportValid,
                                     const QString& reportFragment,
                                     const QString& manifestPath,
                                     qint64 manifestLines,
                                     const QString& manifestFragment);
    /// PLOT-010: mag + residual phase первой valid-строки прямой LUT.
    void applyDirectLutCurvePreview(const QVector<double>& freqGhz,
                                    const QVector<double>& magDb,
                                    const QVector<double>& phaseErrorDeg,
                                    int channel,
                                    int attCode,
                                    int phaseCode);
    void setRunStateGuide(int runState);
    void setUnfinishedSeriesHint(const QString& path);
    void setVnaCalibrationIdHint(const QString& id);

    [[nodiscard]] double fStartHz() const;
    [[nodiscard]] double fStopHz() const;
    [[nodiscard]] int points() const;
    [[nodiscard]] int ifbwHz() const;
    [[nodiscard]] double powerDbm() const;
    [[nodiscard]] int averages() const;

signals:
    void openWizardRequested();
    void resumeSeriesRequested();

private slots:
    void onStageClicked(int row);
    void onFreqSpinChanged();
    void onFreqUnitChanged();
    void onIfbwSpinChanged();
    void onIfbwUnitChanged();

private:
    QWidget* makeConnectionsPage();
    QWidget* makeCalPage();
    QWidget* makeLinearityPage();
    QWidget* makeSweepPage();
    QWidget* makeDirectPage();
    QWidget* makeInversePage();
    QWidget* makeValidationPage();
    void refreshCalPageText();
    void syncFreqSpinsFromHz();
    void syncIfbwSpinFromHz();
    void applyFreqSpinLimits(QDoubleSpinBox* spin, int unitIndex) const;
    [[nodiscard]] static double freqUnitScale(int unitIndex);
    [[nodiscard]] static double ifbwUnitScale(int unitIndex);

    QListWidget* m_stages = nullptr;
    QStackedWidget* m_stack = nullptr;

    QDoubleSpinBox* m_fStart = nullptr;
    QDoubleSpinBox* m_fStop = nullptr;
    QComboBox* m_fStartUnit = nullptr;
    QComboBox* m_fStopUnit = nullptr;
    QSpinBox* m_points = nullptr;
    QDoubleSpinBox* m_ifbw = nullptr;
    QComboBox* m_ifbwUnit = nullptr;
    QDoubleSpinBox* m_power = nullptr;
    QSpinBox* m_averages = nullptr;

    double m_fStartHz = 1.246e9;
    double m_fStopHz = 1.346e9;
    int m_ifbwHz = 1000;
    bool m_freqUiGuard = false;

    QLabel* m_plotState = nullptr;
    QLabel* m_current = nullptr;
    QLabel* m_plotLegend = nullptr;
    QLabel* m_nextHint = nullptr;
    S21PlotWidget* m_plotS11 = nullptr;
    S21PlotWidget* m_plotS21 = nullptr;
    S21PlotWidget* m_plotS12 = nullptr;
    S21PlotWidget* m_plotS22 = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_eta = nullptr;
    QLabel* m_counter = nullptr;

    QLabel* m_connectHow = nullptr;
    QLabel* m_unfinished = nullptr;
    QLabel* m_calText = nullptr;
    QLabel* m_linText = nullptr;
    QLabel* m_directSummary = nullptr;
    QLabel* m_directFragment = nullptr;
    QLabel* m_inverseSummary = nullptr;
    QLabel* m_inverseFragment = nullptr;
    QLabel* m_validSummary = nullptr;
    QLabel* m_validFragment = nullptr;
    S21PlotWidget* m_lutPlot = nullptr;
    QLabel* m_lutPlotCaption = nullptr;

    QString m_vnaCalId;
    bool m_calOk = false;
    QString m_thruText = QStringLiteral("не измерено");
    bool m_connectionsOk = false;
    bool m_idnOk = false;
};
