#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <vector>

class FullScanPanel;
class PsExchangeView;
class PsModulesPage;
class PsSession;
class QDoubleSpinBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

/// Вкладка «Установка параметров» по эскизу: сверху «Режим изделия», «АЦП» и
/// «Луч»; ниже широкое окно «Модули» (гетеродин смесителя, гетеродин
/// калибровки, смеситель, видеоприёмник, коды решётки) с «Записать»,
/// «Прочитать» и ответом; внизу «Обмен с PS | Телеметрия». Команды выполняет
/// PS (docs/contracts/ps-json-protocol.md); во время серии они заблокированы.
class ParametersTab final : public QWidget {
    Q_OBJECT

public:
    [[nodiscard]] FullScanPanel* fullScan() const { return m_fullScan; }

    explicit ParametersTab(PsSession* session, QWidget* parent = nullptr);
    ~ParametersTab() override;

    /// Таблица обмена: строка на каждый отправленный и принятый пакет TCP и UDP.
    [[nodiscard]] QTableWidget* exchangeTable() const;
    [[nodiscard]] PsModulesPage* modulesPage() const { return m_modulesPage; }

    /// Набор кодов 16 каналов в файле — payload set_phase_atten ({"ch": […]}).
    bool saveCodes(const QString& path, QString* error = nullptr) const;
    bool loadCodes(const QString& path, QString* error = nullptr);

private:
    QWidget* buildModeBox();
    QWidget* buildBeamBox();
    QWidget* buildCodesPage();
    QWidget* buildBottomTabs();

    void onResponse(const afar::ps::Envelope& response, afar::ps::Status status);
    void onTelemetry();
    void send(std::string_view msgType, const nlohmann::json& payload = nlohmann::json::object());
    void writeCodes();
    void showCodes(const std::vector<afar::ps::ChannelCodes>& codes);
    void refreshMode();
    void refreshEnabled();
    void log(const QString& text);

    PsSession* m_session;

    QLabel* m_modeLabel = nullptr;
    QPushButton* m_toService = nullptr;
    QPushButton* m_toIdle = nullptr;
    QPushButton* m_getState = nullptr;

    QLabel* m_tlmCounters = nullptr;
    QTableWidget* m_tlmTable = nullptr;

    QTableWidget* m_codes = nullptr;
    QPushButton* m_writeCodes = nullptr;
    QPushButton* m_readCodes = nullptr;
    QSpinBox* m_fillPh = nullptr;
    QSpinBox* m_fillAtt = nullptr;


    QDoubleSpinBox* m_beamAz = nullptr;
    QSpinBox* m_beamId = nullptr;
    QLabel* m_beamState = nullptr;

    PsExchangeView* m_exchange = nullptr;
    QPlainTextEdit* m_log = nullptr;
    FullScanPanel* m_fullScan = nullptr;
    PsModulesPage* m_modulesPage = nullptr;

    std::vector<QWidget*> m_serviceControls;
};
