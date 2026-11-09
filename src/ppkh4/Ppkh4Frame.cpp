#include "Ppkh4Frame.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace afar::ppkh4 {

namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Как BigInt("0x…") в исходной странице: неверная запись даёт ноль.
std::vector<bool> parse_fixed(const std::string& text, int total)
{
    std::vector<bool> bits(static_cast<std::size_t>(total), false);
    std::string hex;
    for (const char c : text) {
        if (c != ' ' && c != '\t') {
            hex.push_back(c);
        }
    }
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.erase(0, 2);
    }
    if (hex.empty()) {
        return bits;
    }
    for (const char c : hex) {
        if (hex_value(c) < 0) {
            return bits;
        }
    }
    int pos = 0;
    for (auto it = hex.rbegin(); it != hex.rend() && pos < total; ++it) {
        const int nibble = hex_value(*it);
        for (int b = 0; b < 4 && pos < total; ++b, ++pos) {
            bits[static_cast<std::size_t>(pos)] = ((nibble >> b) & 1) != 0;
        }
    }
    return bits;
}

int reverse_bits(int value, int width)
{
    int out = 0;
    for (int i = 0; i < width; ++i) {
        if ((value & (1 << i)) != 0) {
            out |= 1 << (width - 1 - i);
        }
    }
    return out;
}

int round_half_up(double x)
{
    return static_cast<int>(std::floor(x + 0.5));
}

}  // namespace

int max_code(int width)
{
    const int w = std::clamp(width, 1, kMaxFieldBits);
    return (1 << w) - 1;
}

int clamp_code(int code, const FieldFormat& field)
{
    return std::clamp(code, 0, max_code(field.width));
}

double phase_deg(int code, const FieldFormat& field)
{
    return code * field.lsb;
}

double atten_db(int code, const FieldFormat& field)
{
    return (code - max_code(field.width)) * field.lsb;
}

int phase_code_from_deg(double deg, const FieldFormat& field)
{
    if (field.lsb == 0.0 || !std::isfinite(deg)) {
        return 0;
    }
    return clamp_code(round_half_up(deg / field.lsb), field);
}

int atten_code_from_db(double db, const FieldFormat& field)
{
    if (field.lsb == 0.0 || !std::isfinite(db)) {
        return max_code(field.width);
    }
    return clamp_code(round_half_up(max_code(field.width) - std::abs(db) / field.lsb), field);
}

std::vector<bool> build_frame(const FrameFormat& format, int phase_code, int atten_code)
{
    const int total = std::clamp(format.total_bits, 1, kMaxFrameBits);
    std::vector<bool> bits = parse_fixed(format.fixed_hex, total);
    auto put = [&bits, total](int code, const FieldFormat& field) {
        const int width = std::clamp(field.width, 1, kMaxFieldBits);
        int value = code & max_code(width);
        if (field.reverse) {
            value = reverse_bits(value, width);
        }
        if (field.invert) {
            value = ~value & max_code(width);
        }
        for (int i = 0; i < width; ++i) {
            const int pos = field.offset + i;
            if (pos >= 0 && pos < total) {
                bits[static_cast<std::size_t>(pos)] = ((value >> i) & 1) != 0;
            }
        }
    };
    put(phase_code, format.phase);
    put(atten_code, format.atten);
    return bits;
}

std::string frame_hex(const std::vector<bool>& bits)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    const std::size_t digits = (bits.size() + 3) / 4;
    std::string out(digits, '0');
    for (std::size_t d = 0; d < digits; ++d) {
        int nibble = 0;
        for (std::size_t b = 0; b < 4; ++b) {
            const std::size_t pos = d * 4 + b;
            if (pos < bits.size() && bits[pos]) {
                nibble |= 1 << b;
            }
        }
        out[digits - 1 - d] = kDigits[nibble];
    }
    return out;
}

bool fields_overlap(const FrameFormat& format)
{
    const auto& p = format.phase;
    const auto& a = format.atten;
    return std::max(p.offset, a.offset) < std::min(p.offset + p.width, a.offset + a.width);
}

bool fields_outside(const FrameFormat& format)
{
    const auto& p = format.phase;
    const auto& a = format.atten;
    return std::max(p.offset + p.width, a.offset + a.width) > format.total_bits;
}

bool format_valid(const FrameFormat& format)
{
    auto field_ok = [](const FieldFormat& f) {
        return f.offset >= 0 && f.width >= 1 && f.width <= kMaxFieldBits;
    };
    return format.total_bits >= 1 && format.total_bits <= kMaxFrameBits && field_ok(format.phase)
           && field_ok(format.atten);
}

std::string send_command(const FrameFormat& format, int phase_code, int atten_code)
{
    const auto bits = build_frame(format, phase_code, atten_code);
    return "SEND " + std::to_string(bits.size()) + " " + frame_hex(bits);
}

std::vector<bool> chain_bits(const FrameFormat& format, std::span<const int> phase,
                            std::span<const int> atten, std::span<const int> lsb_first)
{
    const int width = std::clamp(format.total_bits, 1, kMaxFrameBits);
    const int count = static_cast<int>(std::min(phase.size(), atten.size()));
    const int total = width * count;
    std::vector<bool> bits(static_cast<std::size_t>(std::min(total, kMaxFrameBits)), false);
    FrameFormat one = format;
    one.total_bits = width;
    const int used = static_cast<int>(bits.size()) / width;
    for (int ch = 0; ch < used; ++ch) {
        const auto word = build_frame(one, phase[static_cast<std::size_t>(ch)],
                                       atten[static_cast<std::size_t>(ch)]);
        const bool flip = ch < static_cast<int>(lsb_first.size())
                          && lsb_first[static_cast<std::size_t>(ch)] != 0;
        const int base = ch * width;
        for (int i = 0; i < width; ++i) {
            const int src = flip ? (width - 1 - i) : i;
            bits[static_cast<std::size_t>(base + i)] = word[static_cast<std::size_t>(src)];
        }
    }
    return bits;
}

std::string send_chain_command(const FrameFormat& format, std::span<const int> phase,
                               std::span<const int> atten, std::span<const int> lsb_first)
{
    const auto bits = chain_bits(format, phase, atten, lsb_first);
    return "SEND " + std::to_string(bits.size()) + " " + frame_hex(bits);
}

StatusFrame parse_status(const std::string& line)
{
    StatusFrame status;
    if (line.rfind("OK STATUS", 0) != 0) {
        return status;
    }
    status.ok = true;
    const auto field = [&line](const char* key) -> std::string {
        const auto pos = line.find(key);
        if (pos == std::string::npos) {
            return {};
        }
        const auto value = pos + std::char_traits<char>::length(key);
        const auto end = line.find(' ', value);
        return line.substr(value, end == std::string::npos ? std::string::npos : end - value);
    };
    const std::string bits = field("BITS=");
    const std::string frame = field("FRAME=");
    if (bits.empty() || frame.empty()) {
        status.ok = false;
        return status;
    }
    try {
        status.bits = std::stoi(bits);
    } catch (const std::exception&) {
        status.ok = false;
        return status;
    }
    if (frame == "-") {
        return status;
    }
    status.has_frame = true;
    status.hex = frame;
    return status;
}

bool decode_chain(const FrameFormat& format, const std::string& hex, std::span<int> phase,
                  std::span<int> atten, std::span<const int> lsb_first)
{
    const int width = std::clamp(format.total_bits, 1, kMaxFrameBits);
    const int count = static_cast<int>(std::min(phase.size(), atten.size()));
    if (count <= 0 || hex.empty()) {
        return false;
    }
    const int total = width * count;
    if (static_cast<int>(hex.size()) != (total + 3) / 4) {
        return false;
    }
    const std::vector<bool> bits = parse_fixed(hex, total);
    auto extract = [](const std::vector<bool>& word, const FieldFormat& field) {
        const int field_width = std::clamp(field.width, 1, kMaxFieldBits);
        int value = 0;
        for (int i = 0; i < field_width; ++i) {
            const int pos = field.offset + i;
            if (pos >= 0 && pos < static_cast<int>(word.size()) && word[static_cast<std::size_t>(pos)]) {
                value |= 1 << i;
            }
        }
        if (field.invert) {
            value = ~value & max_code(field_width);
        }
        if (field.reverse) {
            value = reverse_bits(value, field_width);
        }
        return value;
    };
    for (int ch = 0; ch < count; ++ch) {
        const bool flip = ch < static_cast<int>(lsb_first.size()) && lsb_first[static_cast<std::size_t>(ch)] != 0;
        std::vector<bool> word(static_cast<std::size_t>(width), false);
        const int base = ch * width;
        for (int i = 0; i < width; ++i) {
            const int src = flip ? (width - 1 - i) : i;
            word[static_cast<std::size_t>(src)] = bits[static_cast<std::size_t>(base + i)];
        }
        phase[static_cast<std::size_t>(ch)] = extract(word, format.phase);
        atten[static_cast<std::size_t>(ch)] = extract(word, format.atten);
    }
    return true;
}

std::vector<std::string> cfg_commands(const LineConfig& lines)
{
    return {
        "CFG LSBFIRST " + std::to_string(lines.lsb_first),
        "CFG CLKIDLE " + std::to_string(lines.clk_idle),
        "CFG LEACT " + std::to_string(lines.le_active),
        "CFG LEMODE " + std::to_string(lines.le_mode),
        "CFG HALFUS " + std::to_string(lines.half_us),
        "CFG LEUS " + std::to_string(lines.le_us),
    };
}

}  // namespace afar::ppkh4
