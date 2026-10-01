#pragma once

#include <QString>

namespace S2VnaRuntime {

struct StartResult {
    bool ok = false;
    bool started = false;
    QString message;
};

[[nodiscard]] QString defaultExecutablePath();
bool enableSocketServer(const QString& executablePath, QString* error = nullptr);
[[nodiscard]] StartResult ensureRunningHidden(const QString& executablePath);
void hideRunningWindows();

}  // namespace S2VnaRuntime
