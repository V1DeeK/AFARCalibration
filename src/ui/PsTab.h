#pragma once

#include "PsLink.h"
#include "PsProtocol.h"

#include <QWidget>

#include <map>
#include <vector>

class PsPacketPage;
class PsServiceCheckPage;
class PsTrafficView;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTabWidget;

/// Вкладка «Параметры PS»: связь с PS изделия, режим, телеметрия и ручные
/// сервисные команды (docs/contracts/ps-json-protocol.md). Все действия
/// выполняет PS; вкладка только формирует команды и показывает ответы.
/// Программа только клиент PS: подключается по адресу и шлёт JSON-команды.
/// Во время серии ручные команды заблокированы, связь с PS при этом не рвётся.
class PsTab final : public QWidget {
    Q_OBJECT

public:
    explicit PsTab(QWidget* parent = nullptr);
    ~PsTab() override;

    /// Серия идёт — ручные команды запрещены (ТЗ т. 8.2); связь остаётся.
    void setSeriesActive(bool active);
    [[nodiscard]] PsLink* link() const { return m_link; }
    [[nodiscard]] PsServiceCheckPage* checkPage() const { return m_checkPage; }
    [[nodiscard]] PsPacketPage* packetPage() const { return m_packetPage; }
    [[nodiscard]] PsTrafficView* trafficView() const { return m_traffic; }

    /// Набор кодов 16 каналов в файле — payload set_phase_atten ({"ch": […]}).
    bool saveCodes(const QString& path, QString* error = nullptr) const;
    bool loadCodes(const QString& path, QString* error = nullptr);

signals:
    /// Короткий итог для строки соединений главного окна.
    void summaryChanged(const QString& text, bool ok);

private:
    QWidget* buildConnectionBox();
    QWidget* buildModeBox();
    QWidget* buildTelemetryBox();
    QWidget* buildCodesPage();
    QWidget* buildAdcPage();
    QWidget* buildBeamPage();
    QWidget* buildRawPage();

    void onConnectClicked();
    void onLinkStateChanged(PsLink::State state);
    void onResponse(const afar::ps::Envelope& response, afar::ps::Status status);
    void onTelemetry(const afar::ps::Telemetry& t);

    void send(std::string_view msgType, const nlohmann::json& payload = nlohmann::json::object());
    void writeCodes();
    void applyAdc();
    void showCodes(const std::vector<afar::ps::ChannelCodes>& codes);
    void setMode(afar::ps::Mode mode, const QString& state);
    void refreshEnabled();
    void log(const QString& text);

    PsLink* m_link = nullptr;
    bool m_seriesActive = false;
    bool m_modeKnown = false;
    afar::ps::Mode m_mode = afar::ps::Mode::Idle;

    QLineEdit* m_host = nullptr;
    QSpinBox* m_port = nullptr;
    QSpinBox* m_tlmPort = nullptr;
    QSpinBox* m_keepAlive = nullptr;
    QPushButton* m_connect = nullptr;
    QLabel* m_linkState = nullptr;

    QLabel* m_modeLabel = nullptr;
    QPushButton* m_toService = nullptr;
    QPushButton* m_toIdle = nullptr;
    QPushButton* m_getState = nullptr;

    QLabel* m_tlmCounters = nullptr;
    QTableWidget* m_tlmTable = nullptr;

    QTabWidget* m_pages = nullptr;
    QTableWidget* m_codes = nullptr;
    QPushButton* m_writeCodes = nullptr;
    QPushButton* m_readCodes = nullptr;
    QSpinBox* m_fillPh = nullptr;
    QSpinBox* m_fillAtt = nullptr;

    QDoubleSpinBox* m_adcLoMhz = nullptr;
    QDoubleSpinBox* m_adcBwMhz = nullptr;
    QDoubleSpinBox* m_adcFsMhz = nullptr;
    QDoubleSpinBox* m_adcGain = nullptr;
    QLabel* m_adcActual = nullptr;

    QDoubleSpinBox* m_beamAz = nullptr;
    QSpinBox* m_beamId = nullptr;
    QLabel* m_beamState = nullptr;


    QLineEdit* m_rawHex = nullptr;

    PsTrafficView* m_traffic = nullptr;
    QPlainTextEdit* m_log = nullptr;

    PsServiceCheckPage* m_checkPage = nullptr;
    PsPacketPage* m_packetPage = nullptr;

    std::vector<QWidget*> m_serviceControls;
};
