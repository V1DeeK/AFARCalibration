#pragma once

#include <QWizard>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

/// Мастер FR-04 / UI-007 до READY на имитаторах.
class StartWizard final : public QWizard {
    Q_OBJECT

public:
    explicit StartWizard(QWidget* parent = nullptr);

    void setSweepPreset(double fStartGhz,
                        double fStopGhz,
                        int points,
                        int ifbwHz,
                        double powerDbm,
                        int averages);

    [[nodiscard]] QString dataRoot() const;
    [[nodiscard]] QString runConfigPath() const;
    [[nodiscard]] QString attenuatorCsvPath() const;
    [[nodiscard]] double powerDbm() const;
    [[nodiscard]] bool directAccessRequested() const;
    [[nodiscard]] bool forceSafeState() const;
    [[nodiscard]] bool engineerProfile() const;
    [[nodiscard]] bool connectionsConfirmed() const;
    [[nodiscard]] bool idnConfirmed() const;
    [[nodiscard]] bool calConfirmed() const;
    [[nodiscard]] bool noOverloadConfirmed() const;
    [[nodiscard]] bool probeConfirmed() const;
    [[nodiscard]] QString thruSummary() const;

    /// Пишет probe-фикстуры; частоты/точки из пресета вкладки измерения.
    bool materializeSimFixtures(QString& diagnostics);

protected:
    bool validateCurrentPage() override;
    void initializePage(int id) override;

private slots:
    void onHelpRequested();

private:
    void buildPages();
    bool confirmDangerousSettings();
    void applyEngineerGate();

    QLineEdit* m_dataRoot = nullptr;
    QCheckBox* m_powerOk = nullptr;
    QCheckBox* m_engineer = nullptr;
    QCheckBox* m_idnOk = nullptr;
    QCheckBox* m_calOk = nullptr;
    QDoubleSpinBox* m_thruMag = nullptr;
    QDoubleSpinBox* m_thruPhase = nullptr;
    QCheckBox* m_noOverload = nullptr;
    QCheckBox* m_probeOk = nullptr;
    QDoubleSpinBox* m_power = nullptr;
    QCheckBox* m_directAccess = nullptr;
    QCheckBox* m_forceSafe = nullptr;
    QLabel* m_tempHint = nullptr;
    QString m_cfgPath;
    QString m_csvPath;
    double m_fStartGhz{4.9};
    double m_fStopGhz{6.0};
    int m_points{201};
    int m_ifbwHz{1000};
    int m_averages{8};
};
