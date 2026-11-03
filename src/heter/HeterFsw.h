#pragma once

#include <QString>
#include <memory>

class QTcpSocket;

/// Анализатор R&S FSW по SCPI (TCP 5025), как emu/fsw.py в V1DeeK/Heterodyne.
struct HeterPeak {
    double peakHz = 0;
    double levelDbm = 0;
    double contrastDb = 0;
    double spanHz = 0;
    double rbwHz = 0;
    bool signal = false;
};

class HeterFsw {
public:
    explicit HeterFsw(const QString& ip);
    ~HeterFsw();

    HeterFsw(const HeterFsw&) = delete;
    HeterFsw& operator=(const HeterFsw&) = delete;

    [[nodiscard]] QString idn() const { return m_idn; }
    HeterPeak measure(double centerHz, double spanHz);
    QByteArray screenshot();

    /// *IDN? на порту анализатора (5025) или блока питания (5555, затем 5025).
    static QString identify(const QString& ip, const QString& device);
    /// Переносит центр текущего канала FSW на частоту гетеродина, не создавая свой канал.
    static void retune(const QString& ip, double centerHz);

private:
    void command(const QString& text);
    QString query(const QString& text);
    void wait();
    void check();
    void cleanup(const QString& previous);
    QByteArray readBytes(int count);
    std::pair<double, double> sweep(double centerHz, double spanHz, double rbwHz);

    std::unique_ptr<QTcpSocket> m_sock;
    QByteArray m_buf;
    QString m_idn;
    QString m_channel;
    QString m_png;
    QString m_previous;
    bool m_ready = false;
    bool m_hasPng = false;
};
