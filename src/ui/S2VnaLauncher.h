#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QTcpSocket;
class QTimer;

/// Скрытый запуск S2VNA на этом ПК (решение пользователя от 2026-10-16).
/// Только документированные ключи командной строки (РЭ S2VNA ч. 2, стр. 343);
/// файлы настроек S2VNA не правятся. SCPI здесь не посылается: SYST:TERM/SHOW/HIDE
/// идут через MeasureWorker и C2220Vna.
class S2VnaLauncher final : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString executable;
        quint16 port{5025};
        /// Demo Mode S2VNA (`/simulate:on`).
        bool demo{false};
        /// Сколько ждать Socket Server после запуска.
        int startTimeoutMs{60000};
    };

    explicit S2VnaLauncher(QObject* parent = nullptr);
    ~S2VnaLauncher() override;

    /// Ключи запуска: `/visible:off /SocketServer:on /SocketPort:N [/simulate:on]`.
    [[nodiscard]] static QStringList arguments(const Options& options);
    /// Автозапуск возможен только для S2VNA на этом же компьютере.
    [[nodiscard]] static bool isLocalHost(const QString& host);
    [[nodiscard]] static QString defaultExecutablePath();

    /// Если порт уже слушает — S2VNA запущена кем-то ещё, процесс не трогаем.
    /// Иначе запускает S2VNA скрыто и ждёт Socket Server. Итог — сигнал finished().
    void ensureRunning(const Options& options);
    [[nodiscard]] bool busy() const noexcept { return m_phase != Phase::Idle; }
    /// Процесс запущен этой программой и ещё работает.
    [[nodiscard]] bool ownsRunningProcess() const;
    [[nodiscard]] bool ownedDemo() const noexcept { return m_ownedDemo; }
    /// Ждёт завершения своей копии S2VNA (после SYST:TERM); не дождался —
    /// закрывает окно (terminate), затем kill. Чужую S2VNA не трогает.
    void stopOwned(int gracefulWaitMs);

signals:
    void finished(bool ok, const QString& message);

private:
    enum class Phase { Idle, CheckingExisting, Starting };

    void probePort();
    void onProbeResult(bool open);
    void startProcess();
    void finish(bool ok, const QString& message);

    Options m_options;
    Phase m_phase{Phase::Idle};
    QProcess* m_process{nullptr};
    QTcpSocket* m_probe{nullptr};
    QTimer* m_probeTimeout{nullptr};
    QTimer* m_retry{nullptr};
    qint64 m_startedAtMs{0};
    bool m_ownedDemo{false};
};
