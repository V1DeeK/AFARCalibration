#pragma once

#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QStringList>
#include <QVector>

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
    /// Несколько MAC — известные платы: подключаемся к той, которая сейчас
    /// принимает порт. Если отвечают обе, берётся первая в списке.
    void resolve(const QString& mac, quint16 probePort);
    /// ignoreIp — адрес, который только что не ответил: одну запись ARP с ним
    /// не считаем находкой и опрашиваем сеть.
    void resolve(const QStringList& macs, quint16 probePort, const QString& ignoreIp = {});
    /// Прерывает поиск сразу и без сигналов resolved/failed.
    void cancel();

    /// Платы из списка, которые уже есть в таблице ARP: пары «IP, MAC».
    /// Порядок совпадает со списком MAC.
    [[nodiscard]] QVector<QPair<QString, QString>> boardsInArp(const QStringList& macs) const;
    /// Кто из узлов принял TCP. Порядок сохраняется. Адрес чужой подсети,
    /// у которой нет своего интерфейса, не опрашивается.
    [[nodiscard]] QVector<QPair<QString, QString>> liveBoards(const QVector<QPair<QString, QString>>& hits,
                                                              quint16 port) const;

    /// MAC узла с этим IP по таблице ARP или пусто (в формате AA:BB:CC:DD:EE:FF).
    [[nodiscard]] QString macForIp(const QString& ip) const;

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
    /// После неудачного поиска: узлы, у которых открыт опрашиваемый порт (это и есть плата PS),
    /// как пары «IP, MAC»; MAC пуст, если узла нет в таблице ARP.
    void portOpenHosts(const QVector<QPair<QString, QString>>& hosts);

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
    QStringList m_wanted;  ///< нормализованные MAC, первый — предпочтительный
    quint16 m_port = 0;
    QStringList m_candidates;
    int m_nextCandidate = 0;
    QStringList m_openHosts;      ///< узлы, принявшие подключение на порт опроса
    QSet<QTcpSocket*> m_probes;  ///< сокеты опроса, ещё не завершившие попытку
    QTimer* m_sweepTimeout = nullptr;
    ArpReader m_arpReader;
    CandidateProvider m_candidateProvider;
};
