#include "Manifest.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

namespace afar::report {
namespace {

// Компактный SHA-256 (публичный домен, без OpenSSL).
class Sha256 {
public:
    Sha256() { reset(); }

    void update(const std::uint8_t* data, std::size_t len)
    {
        for (std::size_t i = 0; i < len; ++i) {
            data_[datalen_] = data[i];
            ++datalen_;
            if (datalen_ == 64) {
                transform();
                bitlen_ += 512;
                datalen_ = 0;
            }
        }
    }

    void final(std::uint8_t hash[32])
    {
        std::size_t i = datalen_;
        if (datalen_ < 56) {
            data_[i++] = 0x80;
            while (i < 56) {
                data_[i++] = 0x00;
            }
        } else {
            data_[i++] = 0x80;
            while (i < 64) {
                data_[i++] = 0x00;
            }
            transform();
            for (i = 0; i < 56; ++i) {
                data_[i] = 0x00;
            }
        }
        bitlen_ += static_cast<std::uint64_t>(datalen_) * 8ull;
        data_[63] = static_cast<std::uint8_t>(bitlen_);
        data_[62] = static_cast<std::uint8_t>(bitlen_ >> 8);
        data_[61] = static_cast<std::uint8_t>(bitlen_ >> 16);
        data_[60] = static_cast<std::uint8_t>(bitlen_ >> 24);
        data_[59] = static_cast<std::uint8_t>(bitlen_ >> 32);
        data_[58] = static_cast<std::uint8_t>(bitlen_ >> 40);
        data_[57] = static_cast<std::uint8_t>(bitlen_ >> 48);
        data_[56] = static_cast<std::uint8_t>(bitlen_ >> 56);
        transform();
        for (i = 0; i < 4; ++i) {
            hash[i] = (state_[0] >> (24 - i * 8)) & 0xff;
            hash[i + 4] = (state_[1] >> (24 - i * 8)) & 0xff;
            hash[i + 8] = (state_[2] >> (24 - i * 8)) & 0xff;
            hash[i + 12] = (state_[3] >> (24 - i * 8)) & 0xff;
            hash[i + 16] = (state_[4] >> (24 - i * 8)) & 0xff;
            hash[i + 20] = (state_[5] >> (24 - i * 8)) & 0xff;
            hash[i + 24] = (state_[6] >> (24 - i * 8)) & 0xff;
            hash[i + 28] = (state_[7] >> (24 - i * 8)) & 0xff;
        }
    }

private:
    void reset()
    {
        datalen_ = 0;
        bitlen_ = 0;
        state_[0] = 0x6a09e667u;
        state_[1] = 0xbb67ae85u;
        state_[2] = 0x3c6ef372u;
        state_[3] = 0xa54ff53au;
        state_[4] = 0x510e527fu;
        state_[5] = 0x9b05688cu;
        state_[6] = 0x1f83d9abu;
        state_[7] = 0x5be0cd19u;
    }

    static std::uint32_t rotr(std::uint32_t x, std::uint32_t n)
    {
        return (x >> n) | (x << (32 - n));
    }

    void transform()
    {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
            0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
            0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
            0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
            0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
            0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
            0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
            0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
            0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
            0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
            0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

        std::uint32_t m[64];
        for (std::uint32_t i = 0, j = 0; i < 16; ++i, j += 4) {
            m[i] = (static_cast<std::uint32_t>(data_[j]) << 24)
                | (static_cast<std::uint32_t>(data_[j + 1]) << 16)
                | (static_cast<std::uint32_t>(data_[j + 2]) << 8)
                | (static_cast<std::uint32_t>(data_[j + 3]));
        }
        for (std::uint32_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
            const std::uint32_t s1 = rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
            m[i] = m[i - 16] + s0 + m[i - 7] + s1;
        }

        std::uint32_t a = state_[0];
        std::uint32_t b = state_[1];
        std::uint32_t c = state_[2];
        std::uint32_t d = state_[3];
        std::uint32_t e = state_[4];
        std::uint32_t f = state_[5];
        std::uint32_t g = state_[6];
        std::uint32_t h = state_[7];

        for (std::uint32_t i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = h + S1 + ch + k[i] + m[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::uint8_t data_[64]{};
    std::uint32_t datalen_{0};
    std::uint64_t bitlen_{0};
    std::uint32_t state_[8]{};
};

std::string toHex(const std::uint8_t hash[32])
{
    static constexpr char kDig[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[static_cast<std::size_t>(i) * 2] = kDig[(hash[i] >> 4) & 0xf];
        out[static_cast<std::size_t>(i) * 2 + 1] = kDig[hash[i] & 0xf];
    }
    return out;
}

bool collectSeriesFiles(const SeriesDirectory& series,
                        std::vector<std::filesystem::path>& files,
                        std::string& diagnostics)
{
    files.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(series.root(), ec)) {
        diagnostics = "series root is not a directory";
        return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(series.root(), ec)) {
        if (ec) {
            diagnostics = ec.message();
            return false;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto name = entry.path().filename().string();
        if (name == SeriesDirectory::kManifest) {
            continue;
        }
        files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.filename().string() < b.filename().string();
    });
    return true;
}

}  // namespace

std::string sha256BytesHex(const void* data, std::size_t size)
{
    Sha256 ctx;
    ctx.update(static_cast<const std::uint8_t*>(data), size);
    std::uint8_t hash[32];
    ctx.final(hash);
    return toHex(hash);
}

std::string sha256FileHex(const std::filesystem::path& path, std::string& diagnostics)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot open: " + path.string();
        return {};
    }
    Sha256 ctx;
    std::array<char, 8192> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0) {
            ctx.update(reinterpret_cast<const std::uint8_t*>(buf.data()),
                       static_cast<std::size_t>(n));
        }
    }
    if (in.bad()) {
        diagnostics = "read error: " + path.string();
        return {};
    }
    std::uint8_t hash[32];
    ctx.final(hash);
    return toHex(hash);
}

bool writeManifest(const SeriesDirectory& series, std::string& diagnostics)
{
    std::vector<std::filesystem::path> files;
    if (!collectSeriesFiles(series, files, diagnostics)) {
        return false;
    }
    std::ostringstream oss;
    for (const auto& f : files) {
        const auto hex = sha256FileHex(f, diagnostics);
        if (hex.empty()) {
            return false;
        }
        oss << hex << "  " << f.filename().string() << '\n';
    }
    std::ofstream out(series.manifestPath(), std::ios::binary | std::ios::trunc);
    if (!out) {
        diagnostics = "cannot write manifest";
        return false;
    }
    const auto text = oss.str();
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

bool readManifestFile(const std::filesystem::path& path,
                      std::vector<ManifestEntry>& out,
                      std::string& diagnostics)
{
    out.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        diagnostics = "cannot read manifest";
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (line.size() < 66 || line[64] != ' ' || line[65] != ' ') {
            diagnostics = "bad manifest line format";
            return false;
        }
        ManifestEntry e;
        e.hex = line.substr(0, 64);
        e.filename = line.substr(66);
        out.push_back(std::move(e));
    }
    return true;
}

bool verifyManifest(const SeriesDirectory& series, std::string& diagnostics)
{
    std::vector<ManifestEntry> entries;
    if (!readManifestFile(series.manifestPath(), entries, diagnostics)) {
        return false;
    }
    if (entries.empty()) {
        diagnostics = "manifest is empty";
        return false;
    }
    for (const auto& e : entries) {
        const auto path = series.root() / e.filename;
        if (!std::filesystem::is_regular_file(path)) {
            diagnostics = "missing file listed in manifest: " + e.filename;
            return false;
        }
        const auto hex = sha256FileHex(path, diagnostics);
        if (hex.empty()) {
            return false;
        }
        if (hex != e.hex) {
            diagnostics = "sha256 mismatch: " + e.filename;
            return false;
        }
        const auto sz = std::filesystem::file_size(path);
        if (sz == 0 && e.filename != SeriesDirectory::kRunEvents) {
            // run-events может быть пустым в теории; остальные артефакты — нет
            if (e.filename != "run-events.jsonl") {
                diagnostics = "zero-sized file: " + e.filename;
                return false;
            }
        }
    }
    return true;
}

}  // namespace afar::report
