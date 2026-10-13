#pragma once

#include <QWidget>

class FullScanPanel;
class PsJournalDialog;
class PsPacketPage;
class PsSession;
class QGridLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;

/// Вкладка «Подключение» по эскизу: слева сверху — подключение к плате (как
/// окошко «Подключение к PS» РМО: IP, MAC, порты TCP и UDP, статус, кнопки),
/// рядом — техсостояние мозаикой; справа сверху — журнал обмена мозаикой;
/// снизу слева — «Одна команда | Журнал» из «Теста обмена» РМО, снизу справа —
/// полное сканирование. Плата ищется по MAC, если её адрес сменился.
class ConnectionTab final : public QWidget {
    Q_OBJECT

public:
    explicit ConnectionTab(PsSession* session, QWidget* parent = nullptr);

    /// Дополнительная страница в «Тех. состоянии» (параметры платы libiio).
    void addTechPage(QWidget* page, const QString& title);
    [[nodiscard]] PsPacketPage* packetPage() const { return m_packetPage; }
    [[nodiscard]] FullScanPanel* fullScan() const { return m_fullScan; }
    /// Открыть модальное окно «Журнал».
    void openJournal();
    [[nodiscard]] PsJournalDialog* journal() const { return m_journal; }
    /// MAC из поля окна «Подключение» (его же читают «Параметры платы»).
    [[nodiscard]] QString macText() const;

signals:
    /// Короткий итог для строки состояния главного окна.
    void summaryChanged(const QString& text, bool ok);

private:
    QWidget* buildConnectionBox();
    QWidget* buildTechBox();
    QWidget* buildJournalBox();
    QWidget* buildCommandBox();

    void onConnectClicked();
    void refreshConnection();
    void refreshTech();
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

    QTabWidget* m_techTabs = nullptr;
    QLabel* m_techMode = nullptr;
    QGridLayout* m_techTiles = nullptr;
    QLabel* m_techValues = nullptr;

    QLabel* m_tcpOut = nullptr;
    QLabel* m_tcpIn = nullptr;
    QLabel* m_udpIn = nullptr;
    QLabel* m_udpLost = nullptr;
    QLabel* m_errors = nullptr;
    QPlainTextEdit* m_recent = nullptr;
    int m_tcpOutCount = 0;
    int m_tcpInCount = 0;
    int m_errorCount = 0;

    QTabWidget* m_commandTabs = nullptr;
    PsPacketPage* m_packetPage = nullptr;
    FullScanPanel* m_fullScan = nullptr;
    PsJournalDialog* m_journal = nullptr;
};
