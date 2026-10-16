#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>

/// Локальный генератор потока UDP-телеметрии для проверки устойчивости: шлёт
/// корректные 10-байтные датаграммы на порт телеметрии этого же приложения
/// (127.0.0.1), с заданной частотой. Реальную плату он не нагружает — частоту
/// передачи PS из программы изменить нельзя.
class UdpFloodGenerator final : public QObject {
    Q_OBJECT

public:
    explicit UdpFloodGenerator(QObject* parent = nullptr);

    /// perSecond датаграмм в секунду на port; durationMs = 0 — пока не остановят.
    bool start(quint16 port, int perSecond, qint64 durationMs = 0);
    void stop();
    [[nodiscard]] bool running() const { return m_timer.isActive(); }
    [[nodiscard]] quint64 sent() const { return m_sent; }

signals:
    void finished(quint64 sent);

private:
    void tick();

    QUdpSocket m_socket;
    QTimer m_timer;
    QElapsedTimer m_clock;
    quint16 m_port = 0;
    int m_rate = 0;
    qint64 m_durationMs = 0;
    quint64 m_sent = 0;
};
