#pragma once

#include <QWidget>

class FullScanPanel;
class PsJournalDialog;
class PsPacketPage;
class PsSession;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class StatTile;

/// Вкладка «Подключение» по эскизу: слева сверху — подключение к плате (как
/// окошко «Подключение к PS» РМО: IP, MAC, порты TCP/UDP, статус, кнопки) и
/// «Тех. состояние» с параметрами платы; слева снизу — «Одна команда» из «Теста
/// обмена» РМО. Справа — журнал обмена (мозаика счётчиков, последние пакеты,
/// кнопка модального «Журнала») и полное сканирование.
///
/// IP, MAC и порты можно править всегда, пока нет связи, — и во время попыток
/// подключения, и во время поиска платы по MAC.
class ConnectionTab final : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionTab(PsSession* session, QWidget* parent = nullptr);

    /// Содержимое окошка «Тех. состояние» (параметры платы).
    void setTechPage(QWidget* page);
    [[nodiscard]] PsPacketPage* packetPage() const { return m_packetPage; }
    [[nodiscard]] FullScanPanel* fullScan() const { return m_fullScan; }
    /// Открыть модальное окно «Журнал».
    void openJournal();
    [[nodiscard]] PsJournalDialog* journal() const { return m_journal; }
    /// MAC из поля окна «Подключение» (его же читают «Параметры платы»).
    [[nodiscard]] QString macText() const;

private:
    QWidget* buildConnectionBox();
    QWidget* buildTechBox();
    QWidget* buildJournalBox();
    QWidget* buildCommandBox();

    void connectNow();
    void onMainButton();
    void refreshConnection();
    void refreshCounters();

    PsSession* m_session;
    QLineEdit* m_host = nullptr;
    QLineEdit* m_mac = nullptr;
    QSpinBox* m_port = nullptr;
    QSpinBox* m_tlmPort = nullptr;
    QPushButton* m_connect = nullptr;
    QPushButton* m_findByMac = nullptr;
    QLabel* m_linkState = nullptr;
    QLabel* m_message = nullptr;

    QWidget* m_techBody = nullptr;

    StatTile* m_tcpOut = nullptr;
    StatTile* m_tcpIn = nullptr;
    StatTile* m_udpIn = nullptr;
    StatTile* m_udpLost = nullptr;
    StatTile* m_errors = nullptr;
    QPlainTextEdit* m_recent = nullptr;
    int m_tcpOutCount = 0;
    int m_tcpInCount = 0;
    int m_errorCount = 0;

    QSpinBox* m_keepAlive = nullptr;
    QLabel* m_keepAliveHint = nullptr;
    PsPacketPage* m_packetPage = nullptr;
    FullScanPanel* m_fullScan = nullptr;
    PsJournalDialog* m_journal = nullptr;
};
