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
    void setVnaEndpoint(const QString& host, int port);

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
    /// Ручной идентификатор калибровки ВАЦ (RMD-004 / CAL-001); может быть пустым.
    [[nodiscard]] QString vnaCalibrationId() const;

    /// Пишет probe-фикстуры; частоты/точки из пресета вкладки измерения.
    bool materializeSimFixtures(QString& diagnostics);

public slots:
    /// Ответ worker на пробные коды (успех/отказ до «Готово»).
    void onProbeCodesFinished(bool ok, const QString& message);

signals:
    /// Запрос короткого съёма; Cancel мастера сигнал не шлёт.
    void probeCodesRequested(double fStartGhz,
                             double fStopGhz,
                             int points,
                             int ifbwHz,
                             double powerDbm,
                             int averages);

protected:
    bool validateCurrentPage() override;
    void initializePage(int id) override;

private slots:
    void onHelpRequested();
    void onProbeCheckToggled(bool checked);
    void refreshCalStatusLabel();
    void persistVnaCalibrationId();

private:
    void buildPages();
    bool confirmDangerousSettings();
    void applyEngineerGate();
    [[nodiscard]] bool calChecklistComplete() const;

    QLineEdit* m_dataRoot = nullptr;
    QCheckBox* m_powerOk = nullptr;
    QCheckBox* m_engineer = nullptr;
    QCheckBox* m_idnOk = nullptr;
    QLabel* m_calStatus = nullptr;
    QLineEdit* m_vnaCalId = nullptr;
    QCheckBox* m_calStepResponse = nullptr;
    QCheckBox* m_calStepThru = nullptr;
    QCheckBox* m_calStepApplied = nullptr;
    QCheckBox* m_calOk = nullptr;
    QDoubleSpinBox* m_thruMag = nullptr;
    QDoubleSpinBox* m_thruPhase = nullptr;
    QCheckBox* m_noOverload = nullptr;
    QCheckBox* m_probeOk = nullptr;
    QLabel* m_probeStatus = nullptr;
    bool m_probeCodesOk = false;
    bool m_probeCodesPending = false;
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
    QString m_vnaHost{QStringLiteral("127.0.0.1")};
    int m_vnaPort{5025};
};
