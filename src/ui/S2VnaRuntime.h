#pragma once

#include <QString>
#include <QtGlobal>

class S2VnaRuntime final {
public:
    struct StartResult {
        bool ok{false};
        bool started{false};
        QString message;
    };

    S2VnaRuntime() = default;
    ~S2VnaRuntime();
    S2VnaRuntime(const S2VnaRuntime&) = delete;
    S2VnaRuntime& operator=(const S2VnaRuntime&) = delete;

    [[nodiscard]] StartResult ensureRunningHidden(int socketPort);
    [[nodiscard]] bool ownedProcessRunning() const;
    void stopOwned();

private:
#ifdef Q_OS_WIN
    void* m_process{nullptr};
    void* m_job{nullptr};
    unsigned long m_processId{0};
    int m_socketPort{0};
#endif
};
