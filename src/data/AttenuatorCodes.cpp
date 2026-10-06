#include "AttenuatorCodes.h"

#include <fstream>
#include <sstream>
#include <unordered_set>

namespace afar {
namespace {

std::string readUtf8File(const std::filesystem::path& path, std::string& diagnostics)
{
    diagnostics.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot open file: " + path.string();
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF
        && static_cast<unsigned char>(text[1]) == 0xBB
        && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool parseBool(std::string_view s, bool& out, std::string& diagnostics, int line)
{
    if (s == "true" || s == "1") {
        out = true;
        return true;
    }
    if (s == "false" || s == "0") {
        out = false;
        return true;
    }
    diagnostics = "line " + std::to_string(line) + ": enabled must be true/false (got '"
        + std::string(s) + "')";
    return false;
}

bool parseInt(std::string_view s, int& out, std::string& diagnostics, int line, const char* field)
{
    if (s.empty()) {
        diagnostics = "line " + std::to_string(line) + ": empty " + field;
        return false;
    }
    try {
        std::size_t idx = 0;
        const long v = std::stol(std::string(s), &idx, 10);
        if (idx != s.size()) {
            diagnostics = "line " + std::to_string(line) + ": invalid integer for " + field;
            return false;
        }
        out = static_cast<int>(v);
        return true;
    } catch (...) {
        diagnostics = "line " + std::to_string(line) + ": invalid integer for " + field;
        return false;
    }
}

bool parseDouble(std::string_view s, double& out, std::string& diagnostics, int line, const char* field)
{
    if (s.empty()) {
        diagnostics = "line " + std::to_string(line) + ": empty " + field;
        return false;
    }
    try {
        std::size_t idx = 0;
        out = std::stod(std::string(s), &idx);
        if (idx != s.size()) {
            diagnostics = "line " + std::to_string(line) + ": invalid number for " + field;
            return false;
        }
        return true;
    } catch (...) {
        diagnostics = "line " + std::to_string(line) + ": invalid number for " + field;
        return false;
    }
}

}  // namespace

std::size_t AttenuatorCodes::enabledCount() const
{
    std::size_t n = 0;
    for (const auto& row : rows) {
        if (row.enabled) {
            ++n;
        }
    }
    return n;
}

bool AttenuatorCodes::parse(std::string_view utf8_csv, AttenuatorCodes& out, std::string& diagnostics)
{
    diagnostics.clear();
    AttenuatorCodes result;
    std::unordered_set<int> seen_codes;

    std::size_t pos = 0;
    int line_no = 0;
    bool header_seen = false;

    while (pos <= utf8_csv.size()) {
        std::size_t end = utf8_csv.find('\n', pos);
        std::string_view line;
        if (end == std::string_view::npos) {
            line = utf8_csv.substr(pos);
            pos = utf8_csv.size() + 1;
        } else {
            line = utf8_csv.substr(pos, end - pos);
            pos = end + 1;
        }
        ++line_no;
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (!header_seen) {
            if (line != kHeader) {
                diagnostics = "line " + std::to_string(line_no)
                    + ": expected header '" + std::string(kHeader) + "', got '" + std::string(line)
                    + "'";
                return false;
            }
            header_seen = true;
            continue;
        }

        // Split into 4 fields (no quoted commas in this contract).
        std::string_view fields[4];
        int field_i = 0;
        std::size_t start = 0;
        for (std::size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ',') {
                if (field_i >= 4) {
                    diagnostics = "line " + std::to_string(line_no) + ": too many columns";
                    return false;
                }
                fields[field_i++] = trim(line.substr(start, i - start));
                start = i + 1;
            }
        }
        if (field_i != 4) {
            diagnostics = "line " + std::to_string(line_no) + ": expected 4 columns, got "
                + std::to_string(field_i);
            return false;
        }

        AttenuatorCodeRow row;
        if (!parseInt(fields[0], row.att_code, diagnostics, line_no, "att_code")) {
            return false;
        }
        if (row.att_code < 0 || row.att_code > 63) {
            diagnostics = "line " + std::to_string(line_no) + ": att_code out of range 0..63";
            return false;
        }
        if (!seen_codes.insert(row.att_code).second) {
            diagnostics = "line " + std::to_string(line_no) + ": duplicate att_code "
                + std::to_string(row.att_code);
            return false;
        }
        if (!parseDouble(fields[1], row.att_cmd_db, diagnostics, line_no, "att_cmd_db")) {
            return false;
        }
        if (!parseBool(fields[2], row.enabled, diagnostics, line_no)) {
            return false;
        }
        if (!parseInt(fields[3], row.settle_ms, diagnostics, line_no, "settle_ms")) {
            return false;
        }
        if (row.settle_ms < 0) {
            diagnostics = "line " + std::to_string(line_no) + ": settle_ms must be >= 0";
            return false;
        }
        result.rows.push_back(row);
    }

    if (!header_seen) {
        diagnostics = "CSV is empty: missing header";
        return false;
    }
    if (result.rows.empty()) {
        diagnostics = "CSV has header but no data rows";
        return false;
    }

    out = std::move(result);
    return true;
}

bool AttenuatorCodes::loadFromFile(const std::filesystem::path& path,
                                   AttenuatorCodes& out,
                                   std::string& diagnostics)
{
    const std::string text = readUtf8File(path, diagnostics);
    if (text.empty() && !diagnostics.empty()) {
        return false;
    }
    return parse(text, out, diagnostics);
}

}  // namespace afar
