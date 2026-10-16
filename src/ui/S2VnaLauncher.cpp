#include "S2VnaLauncher.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpSocket>
#include <QTimer>

namespace {

constexpr int kProbeTimeoutMs = 700;
constexpr int kRetryIntervalMs = 500;
constexpr int kKillWaitMs = 3000;

}  // namespace

S2VnaLauncher::S2VnaLauncher(QObject* parent)
    : QObject(parent)
{
    m_probeTimeout = new QTimer(this);
    m_probeTimeout->setSingleShot(true);
    connect(m_probeTimeout, &QTimer::timeout, this, [this]() { onProbeResult(false); });
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, &S2VnaLauncher::probePort);
}

S2VnaLauncher::~S2VnaLauncher()
{
    // QProcess в деструкторе убивает процесс; своя копия S2VNA должна была
    // закрыться раньше через stopOwned(), здесь только страховка.
    if (m_process) {
        m_process->disconnect(this);
    }
}

QStringList S2VnaLauncher::arguments(const Options& options)
{
    QStringList args{QStringLiteral("/visible:off"), QStringLiteral("/SocketServer:on"),
                     QStringLiteral("/SocketPort:%1").arg(options.port)};
    if (options.demo) {
        args << QStringLiteral("/simulate:on");
    }
    return args;
}

bool S2VnaLauncher::isLocalHost(const QString& host)
{
    const QString h = host.trimmed();
    if (h.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QHostAddress address(h);
    return !address.isNull() && address.isLoopback();
}

QString S2VnaLauncher::defaultExecutablePath()
{
#ifdef Q_OS_WIN
    return QStringLiteral("C:/VNA/S2VNA/S2VNA.exe");
#else
    return {};
#endif
}

bool S2VnaLauncher::ownsRunningProcess() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void S2VnaLauncher::ensureRunning(const Options& options)
{
    if (busy()) {
        return;
    }
    m_options = options;
    m_phase = Phase::CheckingExisting;
    probePort();
}

void S2VnaLauncher::probePort()
{
    if (!m_probe) {
        m_probe = new QTcpSocket(this);
        connect(m_probe, &QTcpSocket::connected, this, [this]() { onProbeResult(true); });
        connect(m_probe, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) { onProbeResult(false); });
    }
    m_probe->abort();
    m_probeTimeout->start(kProbeTimeoutMs);
    m_probe->connectToHost(QHostAddress::LocalHost, m_options.port);
}

void S2VnaLauncher::onProbeResult(bool open)
{
    if (m_phase == Phase::Idle) {
        return;
    }
    m_probeTimeout->stop();
    // Пробное соединение сразу закрываем: SCPI-сессию открывает MeasureWorker.
    if (m_probe) {
        m_probe->blockSignals(true);
        m_probe->abort();
        m_probe->blockSignals(false);
    }

    if (m_phase == Phase::CheckingExisting) {
        if (open && ownsRunningProcess() && m_ownedDemo != m_options.demo) {
            // Demo Mode меняется только перезапуском S2VNA (РЭ ч. 2, стр. 1099).
            stopOwned(0);
            startProcess();
            return;
        }
        if (open) {
            finish(true, ownsRunningProcess()
                             ? QStringLiteral("S2VNA уже запущена программой")
                             : QStringLiteral("S2VNA уже запущена на этом ПК"));
            return;
        }
        startProcess();
        return;
    }

    if (open) {
        finish(true, m_options.demo
                         ? QStringLiteral("S2VNA Demo запущена скрыто, Socket Server готов")
                         : QStringLiteral("S2VNA запущена скрыто, Socket Server готов"));
        return;
    }
    if (!ownsRunningProcess()) {
        return;  // Отказ уже сообщил обработчик завершения процесса.
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_startedAtMs > m_options.startTimeoutMs) {
        finish(false, QStringLiteral("S2VNA запущена, но Socket Server на порту %1 не ответил "
                                     "за %2 с. Проверьте, что прибор подключён по USB.")
                          .arg(m_options.port)
                          .arg(m_options.startTimeoutMs / 1000));
        return;
    }
    m_retry->start(kRetryIntervalMs);
}

void S2VnaLauncher::startProcess()
{
    const QFileInfo exe(m_options.executable);
    if (m_options.executable.trimmed().isEmpty() || !exe.isFile()) {
        finish(false, QStringLiteral("S2VNA не запущена и не найдена: укажите путь к S2VNA "
                                     "в настройках подключения (сейчас: «%1»).")
                          .arg(m_options.executable));
        return;
    }
    if (ownsRunningProcess()) {
        // Своя копия работает, но порт молчит: прежний режим (Demo/прибор) или зависание.
        stopOwned(0);
    }
    if (!m_process) {
        m_process = new QProcess(this);
        connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart && m_phase == Phase::Starting) {
                finish(false, QStringLiteral("Не удалось запустить S2VNA: %1")
                                  .arg(m_process->errorString()));
            }
        });
        connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            if (m_phase == Phase::Starting) {
                finish(false, QStringLiteral("S2VNA завершилась при запуске (код %1). "
                                             "Запустите её вручную и проверьте сообщения.")
                                  .arg(code));
            }
        });
    }
    m_process->setProgram(exe.absoluteFilePath());
    m_process->setArguments(arguments(m_options));
    m_process->setWorkingDirectory(exe.absolutePath());
    m_process->setProcessChannelMode(QProcess::ForwardedChannels);
    m_phase = Phase::Starting;
    m_ownedDemo = m_options.demo;
    m_startedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_process->start();
    if (m_phase == Phase::Starting) {
        m_retry->start(kRetryIntervalMs);
    }
}

void S2VnaLauncher::stopOwned(int gracefulWaitMs)
{
    if (!ownsRunningProcess()) {
        return;
    }
    if (gracefulWaitMs > 0 && m_process->waitForFinished(gracefulWaitMs)) {
        return;
    }
    // Как закрытие окна оператором (WM_CLOSE на Windows, SIGTERM в Linux).
    m_process->terminate();
    if (m_process->waitForFinished(kKillWaitMs)) {
        return;
    }
    m_process->kill();
    m_process->waitForFinished(kKillWaitMs);
}

void S2VnaLauncher::finish(bool ok, const QString& message)
{
    m_phase = Phase::Idle;
    m_retry->stop();
    m_probeTimeout->stop();
    emit finished(ok, message);
}
