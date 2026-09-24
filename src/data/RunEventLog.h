#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace afar {

enum class EventLevel {
    Debug,
    Info,
    Warning,
    Error,
};

struct RunEvent {
    std::string timestamp_utc;
    EventLevel level{EventLevel::Info};
    std::string component;
    std::string event_code;
    std::string run_id;
    std::optional<int> channel;
    std::optional<int> att_code;
    std::optional<int> phase_code;
    std::optional<int> attempt;
    std::string text;
};

/// Append-only JSONL журнал серии (DATA-007, LOG-01). Без массивов S21.
class RunEventLog {
public:
    RunEventLog() = default;
    ~RunEventLog();

    RunEventLog(const RunEventLog&) = delete;
    RunEventLog& operator=(const RunEventLog&) = delete;
    RunEventLog(RunEventLog&&) noexcept;
    RunEventLog& operator=(RunEventLog&&) noexcept;

    static bool openAppend(const std::filesystem::path& path,
                           RunEventLog& out,
                           std::string& diagnostics);

    void close();
    [[nodiscard]] bool isOpen() const noexcept { return file_.is_open(); }

    bool append(const RunEvent& event, std::string& diagnostics);

    /// Разбор одной строки. Новые имена `timestamp_utc`/`text` и старые `time_utc`/`message`.
    static bool parseLine(std::string_view line, RunEvent& out, std::string& diagnostics);

    /// Чтение журнала серии. Запись по-прежнему только с новыми именами полей.
    static bool load(const std::filesystem::path& path,
                     std::vector<RunEvent>& out,
                     std::string& diagnostics);

    static std::string nowUtcIso8601();
    static const char* levelName(EventLevel level);

private:
    std::ofstream file_;
};

}  // namespace afar
