#pragma once

#include <QString>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

/// Оболочки приёмки AT-12…14: локальный чеклист/статус (не фейковая метрология).
class AcceptanceAtTab final : public QWidget {
    Q_OBJECT

public:
    explicit AcceptanceAtTab(QWidget* parent = nullptr);

private slots:
    void onAt12Start();
    void onAt12Stop();
    void onAt12FixRss();
    void appendAt12Sample();
    void persistAll();

private:
    void loadFromSettings();
    void refreshAt12Status();
    [[nodiscard]] QString createAt12Csv();

    QLabel* m_at12Status = nullptr;
    QPushButton* m_at12Start = nullptr;
    QPushButton* m_at12Stop = nullptr;
    QPushButton* m_at12FixRss = nullptr;
    QLineEdit* m_at12RssMb = nullptr;
    QLabel* m_at12RssNote = nullptr;
    QLabel* m_at12CsvPath = nullptr;
    QCheckBox* m_at12MemoryNote = nullptr;
    QCheckBox* m_at12NotProtocol = nullptr;
    QTimer* m_at12Timer = nullptr;

    QCheckBox* m_at13Thru = nullptr;
    QLineEdit* m_at13CalId = nullptr;
    QCheckBox* m_at13Limits = nullptr;
    QCheckBox* m_at13MeasureLink = nullptr;

    QCheckBox* m_at14Ch1 = nullptr;
    QCheckBox* m_at14Ch8 = nullptr;
    QCheckBox* m_at14Ch16 = nullptr;
    QCheckBox* m_at14Code0 = nullptr;
    QCheckBox* m_at14Code31 = nullptr;
    QCheckBox* m_at14Code63 = nullptr;
    QLabel* m_at14Blocker = nullptr;
};
