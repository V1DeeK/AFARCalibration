#pragma once

#include "PsProtocol.h"

#include <QWidget>

#include <vector>

class FullScanPanel;
class OperationalPanel;
class PsDiagnosticsPage;
class PsExchangeView;
class PsJournalDialog;
class PsModulesPage;
class PsOsdPage;
class PsPacketPage;
class PsSession;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;
class StatTile;

/// Вкладка «Настройки» по эскизу 19.10.2026 — бывшие «Подключение» и «Установка
/// параметров». Сверху в ряд: «Подключение к плате», «Тех. состояние» (Основное,
/// Ошибки, Подробно), «Оперативный режим», «Режим изделия». Ниже слева «Модули»,
/// под ними «Полное сканирование» и рядом «Луч»; справа под верхним рядом —
/// «Журнал» с вкладками «Журнал обмена» (счётчики и таблица), «Телеметрия»,
/// «Одиночное сообщение» (там же кнопка keep_alive). Команды выполняет
/// PS (docs/contracts/ps-ethernet-v8.md); во время серии они заблокированы.
///
/// IP, MAC и порты можно править всегда, пока нет связи, — и во время попыток
/// подключения, и во время поиска платы по MAC.
class SettingsTab final : public QWidget {
    Q_OBJECT

public:
    explicit SettingsTab(PsSession* session, QWidget* parent = nullptr);
    ~SettingsTab() override;

    /// Содержимое окошка «Тех. состояние» (параметры платы).
    void setTechPage(QWidget* page);
    /// Дополнительный раздел под параметрами платы (очередь ошибок SCPI).
    void addTechSection(QWidget* section);
    /// MAC из поля «Подключение к плате» (его же читают «Параметры платы»).
    [[nodiscard]] QString macText() const;

    /// Таблица обмена: строка на каждый отправленный и принятый пакет TCP и UDP.
    [[nodiscard]] QTableWidget* exchangeTable() const;
    /// Вкладки журнала: «Журнал обмена», «Телеметрия», «Одиночное сообщение».
    [[nodiscard]] QTabWidget* journalTabs() const { return m_journalTabs; }
    [[nodiscard]] PsPacketPage* packetPage() const { return m_packetPage; }
    /// Открыть модальное окно «Журнал» (весь обмен и команда одной строкой).
    void openJournal();
    [[nodiscard]] PsJournalDialog* journal() const { return m_journal; }

    [[nodiscard]] FullScanPanel* fullScan() const { return m_fullScan; }
    [[nodiscard]] PsModulesPage* modulesPage() const { return m_modulesPage; }
    [[nodiscard]] PsDiagnosticsPage* diagnosticsPage() const { return m_diagnostics; }
    [[nodiscard]] PsOsdPage* osdPage() const { return m_osd; }
    [[nodiscard]] OperationalPanel* operationalPanel() const { return m_operational; }

    /// Набор кодов 16 каналов в файле — payload set_phase_atten ({"ch": […]}).
    bool saveCodes(const QString& path, QString* error = nullptr) const;
    bool loadCodes(const QString& path, QString* error = nullptr);

    /// set_start_test, как по кнопке «Запустить самопроверку».
    void startSelfTest();
    /// set_reset без вопроса оператору; кнопка «Перезапустить изделие…» сначала спрашивает.
    void resetBoard();

private:
    QWidget* buildConnectionBox();
    QWidget* buildTechBox();
    QWidget* buildModeBox();
    QWidget* buildBeamBox();
    QWidget* buildCodesPage();
    QWidget* buildJournalBox();
    QWidget* buildSingleMessagePage();

    void connectNow();
    void onMainButton();
    void refreshConnection();
    void refreshCounters();

    void onResponse(const afar::ps::Envelope& response, afar::ps::Status status);
    void onTelemetry();
    void send(std::string_view msgType, const nlohmann::json& payload = nlohmann::json::object());
    void writeCodes();
    void showCodes(const std::vector<afar::ps::ChannelCodes>& codes);
    void refreshMode();
    void refreshEnabled();
    void log(const QString& text);

    PsSession* m_session;

    // Подключение к плате
    QLineEdit* m_host = nullptr;
    QLineEdit* m_mac = nullptr;
    QSpinBox* m_port = nullptr;
    QSpinBox* m_tlmPort = nullptr;
    QPushButton* m_connect = nullptr;
    QPushButton* m_findByMac = nullptr;
    QLabel* m_linkState = nullptr;
    QLabel* m_message = nullptr;

    QWidget* m_techBody = nullptr;

    // Режим изделия и луч
    QLabel* m_modeLabel = nullptr;
    QPushButton* m_toService = nullptr;
    QPushButton* m_toOperational = nullptr;
    QPushButton* m_toIdle = nullptr;
    QPushButton* m_getState = nullptr;
    QPushButton* m_selfTest = nullptr;
    QPushButton* m_reset = nullptr;

    QDoubleSpinBox* m_beamAz = nullptr;
    QSpinBox* m_beamId = nullptr;
    QLabel* m_beamState = nullptr;

    // Модули: коды решётки
    QTableWidget* m_codes = nullptr;
    QPushButton* m_writeCodes = nullptr;
    QPushButton* m_readCodes = nullptr;
    QSpinBox* m_fillPh = nullptr;
    QSpinBox* m_fillAtt = nullptr;

    // Журнал
    StatTile* m_tcpOut = nullptr;
    StatTile* m_tcpIn = nullptr;
    StatTile* m_udpIn = nullptr;
    StatTile* m_udpLost = nullptr;
    StatTile* m_messages = nullptr;
    int m_tcpOutCount = 0;
    int m_tcpInCount = 0;
    int m_messageCount = 0;
    QTabWidget* m_journalTabs = nullptr;
    PsExchangeView* m_exchange = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QLabel* m_tlmCounters = nullptr;
    QTableWidget* m_tlmTable = nullptr;
    QSpinBox* m_keepAlive = nullptr;
    QLabel* m_keepAliveHint = nullptr;
    PsPacketPage* m_packetPage = nullptr;
    PsJournalDialog* m_journal = nullptr;

    FullScanPanel* m_fullScan = nullptr;
    PsModulesPage* m_modulesPage = nullptr;
    PsDiagnosticsPage* m_diagnostics = nullptr;
    PsOsdPage* m_osd = nullptr;
    OperationalPanel* m_operational = nullptr;

    std::vector<QWidget*> m_serviceControls;
};
