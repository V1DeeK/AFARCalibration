#pragma once

#include "PsSession.h"

#include <QWidget>

class QCheckBox;
class QPlainTextEdit;
class QTableWidget;
class QTimer;

/// Таблица обмена с PS: строка на каждый пакет TCP и UDP, ушедший и пришедший.
/// Берёт журнал сессии целиком и дописывает новые строки; одну сессию могут
/// показывать несколько таких таблиц на разных вкладках. Новые строки копятся и
/// выводятся пачкой раз в 100 мс (не больше 300 за раз), поэтому поток пакетов
/// не подвешивает окно.
class PsExchangeView final : public QWidget {
    Q_OBJECT

public:
    enum Column { ColTime, ColChannel, ColDir, ColType, ColMsgCount, ColStatus, ColJson, ColCount };

    /// compact — узкая версия для окошка «Журнал обмена»: время, канал, направление и тип;
    /// номер, статус и JSON — в панели под таблицей.
    explicit PsExchangeView(PsSession* session, QWidget* parent = nullptr, bool compact = false);

    [[nodiscard]] QTableWidget* table() const { return m_table; }
    /// Сообщение выбранной строки целиком, JSON с отступами.
    [[nodiscard]] QPlainTextEdit* detail() const { return m_detail; }

private:
    void rebuild();
    void append(const PsSession::LogLine& line);
    void flushQueue();

    PsSession* m_session;
    QTableWidget* m_table = nullptr;
    QCheckBox* m_showUdp = nullptr;
    QPlainTextEdit* m_detail = nullptr;
    QTimer* m_flush = nullptr;
    QVector<PsSession::LogLine> m_queue;
};
