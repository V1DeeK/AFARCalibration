// Тестовая подмена S2VNA для test_s2vna_launcher: принимает только ключи,
// которые передаёт S2VnaLauncher, слушает Socket-порт и выходит по SYST:TERM.
// В программу не входит.

#include <QCoreApplication>
#include <QHostAddress>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (!args.contains(QStringLiteral("/visible:off"))
        || !args.contains(QStringLiteral("/SocketServer:on"))) {
        return 2;
    }
    quint16 port = 0;
    for (const QString& arg : args) {
        if (arg.startsWith(QStringLiteral("/SocketPort:"))) {
            port = static_cast<quint16>(arg.mid(12).toUInt());
        }
    }
    if (port == 0) {
        return 3;
    }
    const bool demo = args.contains(QStringLiteral("/simulate:on"));

    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost, port)) {
        return 4;
    }
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&]() {
        while (QTcpSocket* client = server.nextPendingConnection()) {
            QObject::connect(client, &QTcpSocket::readyRead, client, [client, demo]() {
                while (client->canReadLine()) {
                    const QByteArray line = client->readLine().trimmed();
                    if (line == "SYST:TERM") {
                        QCoreApplication::quit();
                    } else if (line == "*IDN?") {
                        client->write(demo ? "PLANAR,C2220,,26.3\n" : "PLANAR,C2220,12345,26.3\n");
                    }
                }
            });
            QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        }
    });
    return app.exec();
}
