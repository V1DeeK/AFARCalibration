#pragma once

#include "PsSession.h"

#include <QWidget>

class QCheckBox;
class QTableWidget;

/// Таблица обмена с PS: строка на каждый пакет TCP и UDP, ушедший и пришедший.
/// Берёт журнал сессии целиком и дописывает новые строки; одну сессию могут
/// показывать несколько таких таблиц на разных вкладках.
class PsExchangeView final : public QWidget {
    Q_OBJECT

public:
    enum Column { ColTime, ColChannel, ColDir, ColType, ColMsgCount, ColStatus, ColJson, ColCount };

    explicit PsExchangeView(PsSession* session, QWidget* parent = nullptr);

    [[nodiscard]] QTableWidget* table() const { return m_table; }

private:
    void rebuild();
    void append(const PsSession::LogLine& line);

    PsSession* m_session;
    QTableWidget* m_table = nullptr;
    QCheckBox* m_showUdp = nullptr;
};
