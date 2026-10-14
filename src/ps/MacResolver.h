#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>

#include <functional>

class QTcpSocket;
class QTimer;

/// Поиск платы по MAC: IP у платы меняется, MAC постоянный. Сначала таблица
/// ARP узла, при промахе — опрос локальных подсетей (/24 и уже), который
/// заодно наполняет таблицу ARP, и повторный поиск. Перенесено из РМО
/// (src/connection/MacResolver), разбор ARP дополнен форматом macOS и Linux.
class MacResolver final : public QObject {
    Q_OBJECT

public:
    explicit MacResolver(QObject* parent = nullptr);
    ~MacResolver() override;

    /// Подмена источников для автотестов: таблица ARP (MAC → IP) и список адресов
    /// для опроса. По умолчанию — `arp -a` и подсети компьютера.
    using ArpReader = std::function<QHash<QString, QString>()>;
    using CandidateProvider = std::function<QStringList()>;
    void setArpReader(ArpReader reader) { m_arpReader = std::move(reader); }
    void setCandidateProvider(CandidateProvider provider) { m_candidateProvider = std::move(provider); }

    [[nodiscard]] bool busy() const { return m_busy; }
    /// probePort — TCP-порт, на который стучимся при опросе подсети.
    /// Новый поиск отменяет идущий: результат старого уже не придёт.
    void resolve(const QString& mac, quint16 probePort);
    /// Прерывает поиск сразу и без сигналов resolved/failed.
    void cancel();

    /// 12 шестнадцатеричных цифр прописными или пусто, если MAC неверный.
    static QString normalizeMac(const QString& mac);
    /// AA:BB:CC:DD:EE:FF; неверный MAC возвращается как есть.
    static QString formatMac(const QString& mac);
    static bool isValidMac(const QString& mac);
    /// Таблица ARP из вывода `arp -a` (Windows, macOS, Linux): MAC → IP.
    static QHash<QString, QString> parseArpOutput(const QString& output);

signals:
    void resolved(const QString& ip, const QString& mac);
    void failed(const QString& reason);
    void progress(const QString& text);

private:
    static QHash<QString, QString> readSystemArpTable();
    [[nodiscard]] QHash<QString, QString> readArpTable() const;
    [[nodiscard]] QStringList buildCandidates() const;
    [[nodiscard]] static QStringList systemCandidates();
    void startSweep();
    void launchNextProbes();
    void finishProbe(QTcpSocket* socket);
    void finishSweep();

    bool m_busy = false;
    QString m_mac;
    quint16 m_port = 0;
    QStringList m_candidates;
    int m_nextCandidate = 0;
    QSet<QTcpSocket*> m_probes;  ///< сокеты опроса, ещё не завершившие попытку
    QTimer* m_sweepTimeout = nullptr;
    ArpReader m_arpReader;
    CandidateProvider m_candidateProvider;
};
