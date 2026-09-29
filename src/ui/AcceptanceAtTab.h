#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;

/// Оболочки приёмки AT-12…14: локальный чеклист/статус (не фейковая метрология).
class AcceptanceAtTab final : public QWidget {
    Q_OBJECT

public:
    explicit AcceptanceAtTab(QWidget* parent = nullptr);

private slots:
    void onAt12Start();
    void onAt12Stop();
    void onAt12FixRss();
    void persistAll();

private:
    void loadFromSettings();
    void refreshAt12Status();

    QLabel* m_at12Status = nullptr;
    QPushButton* m_at12Start = nullptr;
    QPushButton* m_at12Stop = nullptr;
    QPushButton* m_at12FixRss = nullptr;
    QLineEdit* m_at12RssMb = nullptr;
    QLabel* m_at12RssNote = nullptr;
    QCheckBox* m_at12MemoryNote = nullptr;
    QCheckBox* m_at12NotProtocol = nullptr;

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
