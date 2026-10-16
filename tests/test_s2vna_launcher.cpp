#include <catch2/catch_test_macros.hpp>

#include "S2VnaLauncher.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>

namespace {

QCoreApplication& app()
{
    static int argc = 1;
    static char name[] = "test_s2vna_launcher";
    static char* argv[] = {name, nullptr};
    // Как в qt_test_application.h: не разрушаем до выхода процесса.
    static QCoreApplication& a = *new QCoreApplication(argc, argv);
    return a;
}

bool waitFor(const std::function<bool()>& cond, int timeoutMs = 10000)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

quint16 freePort()
{
    QTcpServer probe;
    REQUIRE(probe.listen(QHostAddress::LocalHost, 0));
    return probe.serverPort();
}

struct Result {
    bool done{false};
    bool ok{false};
    QString message;

    explicit Result(S2VnaLauncher& launcher)
    {
        QObject::connect(&launcher, &S2VnaLauncher::finished, &launcher,
                         [this](bool success, const QString& text) {
                             done = true;
                             ok = success;
                             message = text;
                         });
    }
    void reset() { *this = Result{}; }

private:
    Result() = default;
};

}  // namespace

TEST_CASE("S2VnaLauncher builds only documented command-line keys", "[s2vna]")
{
    S2VnaLauncher::Options options;
    options.port = 5026;
    REQUIRE(S2VnaLauncher::arguments(options)
            == QStringList{QStringLiteral("/visible:off"), QStringLiteral("/SocketServer:on"),
                           QStringLiteral("/SocketPort:5026")});
    options.demo = true;
    REQUIRE(S2VnaLauncher::arguments(options).contains(QStringLiteral("/simulate:on")));
}

TEST_CASE("S2VnaLauncher starts S2VNA only on this computer", "[s2vna]")
{
    REQUIRE(S2VnaLauncher::isLocalHost(QStringLiteral("127.0.0.1")));
    REQUIRE(S2VnaLauncher::isLocalHost(QStringLiteral(" localhost ")));
    REQUIRE(S2VnaLauncher::isLocalHost(QStringLiteral("::1")));
    REQUIRE_FALSE(S2VnaLauncher::isLocalHost(QStringLiteral("192.168.0.20")));
    REQUIRE_FALSE(S2VnaLauncher::isLocalHost(QString()));
}

TEST_CASE("S2VnaLauncher leaves an already running S2VNA alone", "[s2vna]")
{
    app();
    QTcpServer operatorS2Vna;
    REQUIRE(operatorS2Vna.listen(QHostAddress::LocalHost, 0));

    S2VnaLauncher launcher;
    Result result(launcher);
    S2VnaLauncher::Options options;
    options.executable = QStringLiteral("/nonexistent/S2VNA.exe");
    options.port = operatorS2Vna.serverPort();
    launcher.ensureRunning(options);
    REQUIRE(waitFor([&] { return result.done; }));
    REQUIRE(result.ok);
    REQUIRE_FALSE(launcher.ownsRunningProcess());
    launcher.stopOwned(0);
    REQUIRE(operatorS2Vna.isListening());
}

TEST_CASE("S2VnaLauncher reports a missing S2VNA executable", "[s2vna]")
{
    app();
    S2VnaLauncher launcher;
    Result result(launcher);
    S2VnaLauncher::Options options;
    options.executable = QStringLiteral("/nonexistent/S2VNA.exe");
    options.port = freePort();
    launcher.ensureRunning(options);
    REQUIRE(waitFor([&] { return result.done; }));
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.message.contains(QStringLiteral("путь к S2VNA")));
}

TEST_CASE("S2VnaLauncher starts hidden S2VNA, waits for Socket Server and stops it", "[s2vna]")
{
    app();
    S2VnaLauncher launcher;
    Result result(launcher);
    S2VnaLauncher::Options options;
    options.executable = QStringLiteral(AFAR_FAKE_S2VNA);
    options.port = freePort();
    options.startTimeoutMs = 15000;
    launcher.ensureRunning(options);
    REQUIRE(waitFor([&] { return result.done; }, 20000));
    INFO(result.message.toStdString());
    REQUIRE(result.ok);
    REQUIRE(launcher.ownsRunningProcess());
    REQUIRE_FALSE(launcher.ownedDemo());

    result.reset();
    launcher.ensureRunning(options);
    REQUIRE(waitFor([&] { return result.done; }));
    REQUIRE(result.ok);
    REQUIRE(result.message.contains(QStringLiteral("программой")));

    // Штатный выход как после SYST:TERM от MeasureWorker.
    QTcpSocket scpi;
    scpi.connectToHost(QHostAddress::LocalHost, options.port);
    REQUIRE(scpi.waitForConnected(3000));
    scpi.write("SYST:TERM\n");
    REQUIRE(scpi.waitForBytesWritten(3000));
    launcher.stopOwned(10000);
    REQUIRE_FALSE(launcher.ownsRunningProcess());
}

TEST_CASE("S2VnaLauncher restarts its own S2VNA when Demo Mode changes", "[s2vna]")
{
    app();
    S2VnaLauncher launcher;
    Result result(launcher);
    S2VnaLauncher::Options options;
    options.executable = QStringLiteral(AFAR_FAKE_S2VNA);
    options.port = freePort();
    options.startTimeoutMs = 15000;
    launcher.ensureRunning(options);
    REQUIRE(waitFor([&] { return result.done; }, 20000));
    REQUIRE(result.ok);

    result.reset();
    options.demo = true;
    launcher.ensureRunning(options);
    REQUIRE(waitFor([&] { return result.done; }, 20000));
    INFO(result.message.toStdString());
    REQUIRE(result.ok);
    REQUIRE(launcher.ownedDemo());
    launcher.stopOwned(0);
    REQUIRE_FALSE(launcher.ownsRunningProcess());
}
