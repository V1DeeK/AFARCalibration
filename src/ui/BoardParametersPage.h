#pragma once

#include <QString>
#include <QWidget>

#include <functional>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTimer;
class SpectrumEngine;

/// «Параметры платы» в окошке «Тех. состояние»: безопасное чтение параметров
/// через libiio по адресу платы, на связи с которой программа уже работает (если связи
/// нет — плата сначала ищется по MAC) (модуль «Анализатор спектра»). Настройки платы
/// не меняются. MAC берётся из окошка «Подключение» — плата одна и та же.
class BoardParametersPage final : public QWidget {
    Q_OBJECT

public:
    using MacProvider = std::function<QString()>;
    /// Адрес платы, если она уже на связи по PS (иначе пустая строка — тогда плата ищется по MAC).
    using IpProvider = std::function<QString()>;

    BoardParametersPage(SpectrumEngine& engine, MacProvider macProvider, IpProvider ipProvider,
                        QWidget* parent = nullptr);

    /// Запустить поиск и чтение, как по кнопке «Получить параметры».
    void requestRead();
    [[nodiscard]] bool busy() const { return m_busy; }

signals:
    /// Ход операции для полосы состояния главного окна.
    void operationProgress(const QString& text, int completed, int total);

private:
    void poll();

    SpectrumEngine& m_engine;
    MacProvider m_macProvider;
    IpProvider m_ipProvider;
    QLabel* m_status = nullptr;
    QPushButton* m_read = nullptr;
    QTabWidget* m_results = nullptr;
    QPlainTextEdit* m_main = nullptr;
    QPlainTextEdit* m_details = nullptr;
    QTimer* m_timer = nullptr;
    QString m_lastParameters;
    bool m_busy = false;
};
