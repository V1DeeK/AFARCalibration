#pragma once

#include <QVector>
#include <QWidget>

class QComboBox;
class QGridLayout;
class QLabel;
class QPushButton;
class S21PlotWidget;

/// Матрица 64 фазовых кодов (UI-03 / UI-005).
class CodeMatrixTab final : public QWidget {
    Q_OBJECT

public:
    explicit CodeMatrixTab(QWidget* parent = nullptr);

    void setChannelAttChoices(const QVector<int>& channels, const QVector<int>& attCodes);
    void applySnapshot(int channel,
                       int attCode,
                       int measuringPhase,
                       const QVector<int>& statuses,
                       const QVector<int>& attempts,
                       const QVector<int>& overloadFlags);

    [[nodiscard]] int selectedChannel() const;
    [[nodiscard]] int selectedAttCode() const;
    [[nodiscard]] int selectedPhase() const;
    void setCellSweepCurves(const QVector<double>& freqGhz,
                            const QVector<double>& magDb,
                            const QVector<double>& phaseUnwrapDeg);
    void refreshTheme();

signals:
    void selectionChanged(int channel, int attCode);
    /// FR-18: повтор через оркестратор (не SCPI из GUI).
    void remeasureRequested(int channel, int attCode, const QVector<int>& phases);
    void cellInspectRequested(int channel, int attCode, int phase);

private slots:
    void onSelectionEdited();
    void onCellClicked();
    void onRetrySelected();
    void onRetryRow();

private:
    void updateLegend();

    QComboBox* m_channel = nullptr;
    QComboBox* m_att = nullptr;
    QGridLayout* m_grid = nullptr;
    QVector<QPushButton*> m_cells;
    QLabel* m_detail = nullptr;
    QLabel* m_legend = nullptr;
    S21PlotWidget* m_miniPlot = nullptr;
    QPushButton* m_retrySelected = nullptr;
    QPushButton* m_retryRow = nullptr;
    int m_selectedPhase = 0;
};
