#include "RunEventLog.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace afar {

RunEventLog::~RunEventLog()
{
    close();
}

RunEventLog::RunEventLog(RunEventLog&& other) noexcept
{
    *this = std::move(other);
}

RunEventLog& RunEventLog::operator=(RunEventLog&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    close();
    file_ = std::move(other.file_);
    path_ = std::move(other.path_);
    rotate_bytes_ = other.rotate_bytes_;
    current_size_ = other.current_size_;
    next_rotation_index_ = other.next_rotation_index_;
    return *this;
}

void RunEventLog::close()
{
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

bool RunEventLog::openAppend(const std::filesystem::path& path,
                             RunEventLog& out,
                             std::string& diagnostics,
                             std::uintmax_t rotate_bytes)
{
    diagnostics.clear();
    out.close();
    out.path_ = path;
    out.rotate_bytes_ = rotate_bytes;
    out.current_size_ = 0;
    out.next_rotation_index_ = 1;
    const auto prefix = path.stem().string() + ".";
    const auto suffix = path.extension().string();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
            continue;
        }
        const auto number = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        try {
            out.next_rotation_index_ =
                std::max(out.next_rotation_index_, static_cast<unsigned>(std::stoul(number) + 1));
        } catch (...) {
            continue;
        }
    }
    if (std::filesystem::is_regular_file(path, ec)) {
        out.current_size_ = std::filesystem::file_size(path, ec);
    }
    out.file_.open(path, std::ios::binary | std::ios::app);
    if (!out.file_) {
        diagnostics = "cannot open run-events.jsonl: " + path.string();
        return false;
    }
    return true;
}

bool RunEventLog::rotateIfNeeded(std::size_t additional_bytes, std::string& diagnostics)
{
    if (rotate_bytes_ == 0 || current_size_ == 0
        || current_size_ + additional_bytes <= rotate_bytes_) {
        return true;
    }
    close();
    std::filesystem::path rotated;
    do {
        std::ostringstream name;
        name << path_.stem().string() << '.' << std::setw(4) << std::setfill('0')
             << next_rotation_index_++ << path_.extension().string();
        rotated = path_.parent_path() / name.str();
    } while (std::filesystem::exists(rotated));

    std::error_code ec;
    std::filesystem::rename(path_, rotated, ec);
    if (ec) {
        diagnostics = "RunEventLog rotation failed: " + ec.message();
        return false;
    }
    current_size_ = 0;
    file_.open(path_, std::ios::binary | std::ios::app);
    if (!file_) {
        diagnostics = "cannot reopen run-events.jsonl after rotation";
        return false;
    }
    return true;
}

const char* RunEventLog::levelName(EventLevel level)
{
    switch (level) {
    case EventLevel::Debug:
        return "debug";
    case EventLevel::Info:
        return "info";
    case EventLevel::Warning:
        return "warning";
    case EventLevel::Error:
        return "error";
    }
    return "info";
}

std::string RunEventLog::nowUtcIso8601()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch())
                        .count()
        % 1000;
    std::tm tm_utc{};
#if defined(_WIN32)
    gmtime_s(&tm_utc, &secs);
#else
    gmtime_r(&secs, &tm_utc);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S") << '.'
        << std::setw(3) << std::setfill('0') << ms << 'Z';
    return oss.str();
}

bool RunEventLog::append(const RunEvent& event, std::string& diagnostics)
{
    diagnostics.clear();
    if (!file_.is_open()) {
        diagnostics = "RunEventLog is not open";
        return false;
    }
    nlohmann::json j;
    j["timestamp_utc"] =
        event.timestamp_utc.empty() ? nowUtcIso8601() : event.timestamp_utc;
    j["level"] = levelName(event.level);
    j["component"] = event.component;
    j["event_code"] = event.event_code;
    j["run_id"] = event.run_id;
    if (event.channel) {
        j["channel"] = *event.channel;
    } else {
        j["channel"] = nullptr;
    }
    if (event.att_code) {
        j["att_code"] = *event.att_code;
    } else {
        j["att_code"] = nullptr;
    }
    if (event.phase_code) {
        j["phase_code"] = *event.phase_code;
    } else {
        j["phase_code"] = nullptr;
    }
    if (event.attempt) {
        j["attempt"] = *event.attempt;
    } else {
        j["attempt"] = nullptr;
    }
    j["text"] = event.text;
    const std::string line = j.dump() + '\n';
    if (!rotateIfNeeded(line.size(), diagnostics)) {
        return false;
    }
    file_ << line;
    file_.flush();
    if (!file_) {
        diagnostics = "RunEventLog append failed";
        return false;
    }
    current_size_ += line.size();
    return true;
}

namespace {

std::string jsonStringField(const nlohmann::json& j, const char* primary, const char* legacy)
{
    if (j.contains(primary) && j[primary].is_string()) {
        return j[primary].get<std::string>();
    }
    if (legacy != nullptr && j.contains(legacy) && j[legacy].is_string()) {
        return j[legacy].get<std::string>();
    }
    return {};
}

std::optional<int> jsonOptInt(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || j[key].is_null() || !j[key].is_number_integer()) {
        return std::nullopt;
    }
    return j[key].get<int>();
}

afar::EventLevel levelFromName(std::string_view name)
{
    if (name == "debug") {
        return afar::EventLevel::Debug;
    }
    if (name == "warning") {
        return afar::EventLevel::Warning;
    }
    if (name == "error") {
        return afar::EventLevel::Error;
    }
    return afar::EventLevel::Info;
}

}  // namespace

bool RunEventLog::parseLine(std::string_view line, RunEvent& out, std::string& diagnostics)
{
    diagnostics.clear();
    const auto parsed = nlohmann::json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        diagnostics = "run-events line is not a JSON object";
        return false;
    }
    out = RunEvent{};
    out.timestamp_utc = jsonStringField(parsed, "timestamp_utc", "time_utc");
    out.text = jsonStringField(parsed, "text", "message");
    out.component = jsonStringField(parsed, "component", nullptr);
    out.event_code = jsonStringField(parsed, "event_code", nullptr);
    out.run_id = jsonStringField(parsed, "run_id", nullptr);
    out.level = levelFromName(jsonStringField(parsed, "level", nullptr));
    out.channel = jsonOptInt(parsed, "channel");
    out.att_code = jsonOptInt(parsed, "att_code");
    out.phase_code = jsonOptInt(parsed, "phase_code");
    out.attempt = jsonOptInt(parsed, "attempt");
    if (out.timestamp_utc.empty()) {
        diagnostics = "run-events line has no timestamp_utc or time_utc";
        return false;
    }
    return true;
}

bool RunEventLog::load(const std::filesystem::path& path,
                       std::vector<RunEvent>& out,
                       std::string& diagnostics)
{
    diagnostics.clear();
    out.clear();
    std::vector<std::filesystem::path> files;
    const auto prefix = path.stem().string() + ".";
    const auto suffix = path.extension().string();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path(), ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name != path.filename().string() && name.starts_with(prefix)
            && name.ends_with(suffix)) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    files.push_back(path);
    std::size_t line_no = 0;
    for (const auto& file : files) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            diagnostics = "cannot open run-events file: " + file.string();
            return false;
        }
        std::string line;
        while (std::getline(in, line)) {
            ++line_no;
            if (line.empty() || line == "\r") {
                continue;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            RunEvent ev;
            if (!parseLine(line, ev, diagnostics)) {
                diagnostics = file.filename().string() + " line " + std::to_string(line_no)
                    + ": " + diagnostics;
                out.clear();
                return false;
            }
            out.push_back(std::move(ev));
        }
    }
    return true;
}

}  // namespace afar
