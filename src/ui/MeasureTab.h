#pragma once

#include <QVector>
#include <QWidget>

class QDoubleSpinBox;
class QComboBox;
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

    void applyRunConfigDefaults(double fStartGhz,
                                double fStopGhz,
                                int points,
                                int ifbwHz,
                                double powerDbm,
                                int averages);
    void setStageHighlight(int stageIndex0);
    void setProgress(qint64 completed, qint64 total, int channel, int attCode, int phaseCode);
    void setEtaText(const QString& text);
    void setSweepCurves(const QVector<double>& freqGhz,
                        const QVector<double>& magDb,
                        const QVector<double>& phaseUnwrapDeg);
    void setFilterMetrics(const QString& text);
    void setCalibrationStatus(const QString& text);
    void setCsvAvailable(bool available);
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
    void setUnfinishedSeriesHint(const QString& path);

    [[nodiscard]] double fStartGhz() const;
    [[nodiscard]] double fStopGhz() const;
    [[nodiscard]] int points() const;
    [[nodiscard]] int ifbwHz() const;
    [[nodiscard]] double powerDbm() const;
    [[nodiscard]] int averages() const;
    [[nodiscard]] QString sParameter() const;

signals:
    void openWizardRequested();
    void resumeSeriesRequested();
    void singleSweepRequested();
    void saveCsvRequested();
    void calibrationConnectionRequested();
    void calibrationStepRequested(int step);

private slots:
    void onStageClicked(int row);
    void onFrequencyUnitChanged(int index);

private:
    QWidget* makeConnectionsPage();
    QWidget* makeCalPage();
    QWidget* makeLinearityPage();
    QWidget* makeSweepPage();
    QWidget* makeDirectPage();
    QWidget* makeInversePage();
    QWidget* makeValidationPage();

    QListWidget* m_stages = nullptr;
    QStackedWidget* m_stack = nullptr;

    QDoubleSpinBox* m_fStart = nullptr;
    QDoubleSpinBox* m_fStop = nullptr;
    QComboBox* m_frequencyUnit = nullptr;
    QComboBox* m_sParameter = nullptr;
    QSpinBox* m_points = nullptr;
    QSpinBox* m_ifbw = nullptr;
    QDoubleSpinBox* m_power = nullptr;
    QSpinBox* m_averages = nullptr;
    QLabel* m_current = nullptr;
    S21PlotWidget* m_plot = nullptr;
    QProgressBar* m_progress = nullptr;
    QLabel* m_eta = nullptr;
    QLabel* m_counter = nullptr;
    QLabel* m_filterMetrics = nullptr;
    QPushButton* m_saveCsv = nullptr;
    double m_frequencyScale = 1.0e9;

    QLabel* m_connectHow = nullptr;
    QLabel* m_unfinished = nullptr;
    QLabel* m_calText = nullptr;
    QLabel* m_calibrationStatus = nullptr;
    QLabel* m_linText = nullptr;
    QLabel* m_directText = nullptr;
    QLabel* m_inverseText = nullptr;
    QLabel* m_validText = nullptr;
};
