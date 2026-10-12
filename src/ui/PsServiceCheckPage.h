#pragma once

#include <QWidget>

class PsLink;
class PsServiceCheckRunner;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

/// Страница «Приёмка»: автопрогон сервисных команд PS по контракту с отчётом.
class PsServiceCheckPage final : public QWidget {
    Q_OBJECT

public:
    explicit PsServiceCheckPage(PsLink* link, QWidget* parent = nullptr);

    /// Связь готова и серия не идёт.
    void setAllowed(bool allowed);
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] PsServiceCheckRunner* runner() const { return m_runner; }
    /// Сохранить отчёт Markdown; false и текст в error при ошибке записи.
    bool saveReport(const QString& path, QString* error = nullptr) const;

signals:
    void runningChanged(bool running);

private:
    void onStepStarted(int index);
    void onStepFinished(int index);
    void onFinished(const QString& reason);
    void showPackets(int row);
    void refreshSummary(const QString& tail = {});
    void refreshButtons();

    PsServiceCheckRunner* m_runner = nullptr;
    bool m_allowed = false;
    QPushButton* m_start = nullptr;
    QPushButton* m_stop = nullptr;
    QPushButton* m_save = nullptr;
    QLabel* m_summary = nullptr;
    QTableWidget* m_table = nullptr;
    QPlainTextEdit* m_request = nullptr;
    QPlainTextEdit* m_response = nullptr;
};
