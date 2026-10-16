#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <functional>
#include <optional>

class PsSession;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTableWidget;

/// «Полное сканирование» (режим FULL_SCAN_FREQ, контракт §4.6) — как в консоли
/// PS РМО: частоты и сектор обзора, период, «Применить параметры», «Старт»,
/// «Стоп» и результаты по частотам из send_sff_result. Если изделие не в
/// режиме полного сканирования, панель сначала переводит его туда.
class FullScanPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FullScanPanel(PsSession* session, QWidget* parent = nullptr);

    /// Параметры с формы; false и текст в error — значения вне диапазона.
    bool readParams(afar::ps::SffParams& out, QString* error = nullptr) const;

private:
    void apply();
    void start();
    void stop();
    /// Выполнить действие в режиме FULL_SCAN_FREQ, при необходимости сменив режим.
    void inScanMode(std::function<void()> action);
    void refreshControls();
    void refreshParams();
    void refreshResults();

    PsSession* m_session;
    QDoubleSpinBox* m_freqFrom = nullptr;
    QComboBox* m_freqFromUnit = nullptr;
    QDoubleSpinBox* m_freqTo = nullptr;
    QComboBox* m_freqToUnit = nullptr;
    QDoubleSpinBox* m_azFrom = nullptr;
    QDoubleSpinBox* m_azTo = nullptr;
    QPushButton* m_apply = nullptr;
    QPushButton* m_start = nullptr;
    QPushButton* m_stop = nullptr;
    QLabel* m_state = nullptr;
    QTableWidget* m_results = nullptr;
    std::function<void()> m_afterModeChange;
};
