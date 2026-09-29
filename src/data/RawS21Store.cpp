#include "RawS21Store.h"

#include <cstring>
#include <utility>

namespace afar {
namespace {

template <typename T>
bool writePod(std::fstream& f, const T& v)
{
    f.write(reinterpret_cast<const char*>(&v), sizeof(T));
    return static_cast<bool>(f);
}

template <typename T>
bool readPod(std::fstream& f, T& v)
{
    f.read(reinterpret_cast<char*>(&v), sizeof(T));
    return static_cast<bool>(f);
}

template <typename T>
bool writeVec(std::fstream& f, const std::vector<T>& v)
{
    if (v.empty()) {
        return true;
    }
    f.write(reinterpret_cast<const char*>(v.data()),
            static_cast<std::streamsize>(v.size() * sizeof(T)));
    return static_cast<bool>(f);
}

template <typename T>
bool readVec(std::fstream& f, std::vector<T>& v, std::size_t n)
{
    v.resize(n);
    if (n == 0) {
        return true;
    }
    f.read(reinterpret_cast<char*>(v.data()),
           static_cast<std::streamsize>(n * sizeof(T)));
    return static_cast<bool>(f);
}

bool writeUtf8(std::fstream& f, const std::string& s)
{
    const auto n = static_cast<std::uint32_t>(s.size());
    if (!writePod(f, n)) {
        return false;
    }
    if (n == 0) {
        return true;
    }
    f.write(s.data(), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

bool readUtf8(std::fstream& f, std::string& s)
{
    std::uint32_t n = 0;
    if (!readPod(f, n)) {
        return false;
    }
    s.resize(n);
    if (n == 0) {
        return true;
    }
    f.read(s.data(), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

bool writeMeta(std::fstream& f, const RawS21Meta& meta)
{
    return writeUtf8(f, meta.run_config_json) && writeUtf8(f, meta.vna_idn)
        && writeUtf8(f, meta.vna_calibration_id);
}

bool readMeta(std::fstream& f, RawS21Meta& meta)
{
    return readUtf8(f, meta.run_config_json) && readUtf8(f, meta.vna_idn)
        && readUtf8(f, meta.vna_calibration_id);
}

}  // namespace

std::uint64_t RawS21Store::computeStateStride(std::uint32_t n_freq)
{
    // re[n] f64 + im[n] f64 + temp f32 + overload u8 + valid[n] u8 + attempt u16 + completed u8
    return static_cast<std::uint64_t>(n_freq) * 8ull * 2ull
        + 4ull + 1ull + static_cast<std::uint64_t>(n_freq) + 2ull + 1ull;
}

std::uint64_t RawS21Store::computeRefStride(std::uint32_t n_freq)
{
    // re[n] + im[n] + written u8
    return static_cast<std::uint64_t>(n_freq) * 8ull * 2ull + 1ull;
}

RawS21Store::~RawS21Store()
{
    close();
}

RawS21Store::RawS21Store(RawS21Store&& other) noexcept
{
    *this = std::move(other);
}

RawS21Store& RawS21Store::operator=(RawS21Store&& other) noexcept
{
    if (this == &other) {
        return *this;
    }
    close();
    file_ = std::move(other.file_);
    path_ = std::move(other.path_);
    n_channel_ = other.n_channel_;
    n_att_ = other.n_att_;
    n_phase_ = other.n_phase_;
    n_freq_ = other.n_freq_;
    channels_ = std::move(other.channels_);
    att_codes_ = std::move(other.att_codes_);
    phase_codes_ = std::move(other.phase_codes_);
    frequency_hz_ = std::move(other.frequency_hz_);
    meta_ = std::move(other.meta_);
    states_offset_ = other.states_offset_;
    refs_offset_ = other.refs_offset_;
    state_stride_ = other.state_stride_;
    ref_stride_ = other.ref_stride_;
    other.n_channel_ = other.n_att_ = other.n_phase_ = other.n_freq_ = 0;
    other.states_offset_ = other.refs_offset_ = other.state_stride_ = other.ref_stride_ = 0;
    other.meta_ = {};
    return *this;
}

void RawS21Store::close()
{
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

bool RawS21Store::create(const std::filesystem::path& path,
                         const std::vector<std::uint8_t>& channels,
                         const std::vector<std::uint16_t>& att_codes,
                         const std::vector<std::uint8_t>& phase_codes,
                         const std::vector<std::uint64_t>& frequency_hz,
                         const RawS21Meta& meta,
                         RawS21Store& out,
                         std::string& diagnostics)
{
    diagnostics.clear();
    if (channels.empty() || att_codes.empty() || phase_codes.empty() || frequency_hz.empty()) {
        diagnostics = "RawS21Store::create: empty axes";
        return false;
    }

    out.close();
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!f) {
        // trunc may need out-only first on some platforms
        f.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
        f.close();
        f.open(path, std::ios::binary | std::ios::in | std::ios::out);
    }
    if (!f) {
        diagnostics = "cannot create file: " + path.string();
        return false;
    }

    const std::uint32_t n_ch = static_cast<std::uint32_t>(channels.size());
    const std::uint32_t n_att = static_cast<std::uint32_t>(att_codes.size());
    const std::uint32_t n_ph = static_cast<std::uint32_t>(phase_codes.size());
    const std::uint32_t n_f = static_cast<std::uint32_t>(frequency_hz.size());
    const auto state_stride = computeStateStride(n_f);
    const auto ref_stride = computeRefStride(n_f);

    if (!f.write(kMagic, 8)) {
        diagnostics = "write magic failed";
        return false;
    }
    const std::uint32_t ver = kVersion;
    if (!writePod(f, ver) || !writePod(f, n_ch) || !writePod(f, n_att) || !writePod(f, n_ph)
        || !writePod(f, n_f)) {
        diagnostics = "write dims failed";
        return false;
    }
    if (!writeVec(f, channels) || !writeVec(f, att_codes) || !writeVec(f, phase_codes)
        || !writeVec(f, frequency_hz)) {
        diagnostics = "write axes failed";
        return false;
    }
    if (!writeMeta(f, meta)) {
        diagnostics = "write meta failed";
        return false;
    }

    const auto states_offset = static_cast<std::uint64_t>(f.tellp());
    const std::size_t n_states = static_cast<std::size_t>(n_ch) * n_att * n_ph;
    std::vector<char> zero(static_cast<std::size_t>(state_stride), 0);
    for (std::size_t i = 0; i < n_states; ++i) {
        if (!f.write(zero.data(), static_cast<std::streamsize>(zero.size()))) {
            diagnostics = "preallocate states failed";
            return false;
        }
    }

    const auto refs_offset = static_cast<std::uint64_t>(f.tellp());
    // reference_s21: [n_channel, n_att+1, n_freq]
    const std::size_t n_refs = static_cast<std::size_t>(n_ch) * (static_cast<std::size_t>(n_att) + 1);
    std::vector<char> zero_ref(static_cast<std::size_t>(ref_stride), 0);
    for (std::size_t i = 0; i < n_refs; ++i) {
        if (!f.write(zero_ref.data(), static_cast<std::streamsize>(zero_ref.size()))) {
            diagnostics = "preallocate references failed";
            return false;
        }
    }
    f.flush();

    out.file_ = std::move(f);
    out.path_ = path;
    out.n_channel_ = n_ch;
    out.n_att_ = n_att;
    out.n_phase_ = n_ph;
    out.n_freq_ = n_f;
    out.channels_ = channels;
    out.att_codes_ = att_codes;
    out.phase_codes_ = phase_codes;
    out.frequency_hz_ = frequency_hz;
    out.meta_ = meta;
    out.states_offset_ = states_offset;
    out.refs_offset_ = refs_offset;
    out.state_stride_ = state_stride;
    out.ref_stride_ = ref_stride;
    return true;
}

bool RawS21Store::open(const std::filesystem::path& path,
                       RawS21Store& out,
                       std::string& diagnostics)
{
    diagnostics.clear();
    out.close();
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        diagnostics = "cannot open file: " + path.string();
        return false;
    }

    char magic[8]{};
    f.read(magic, 8);
    if (!f || std::memcmp(magic, kMagic, 8) != 0) {
        diagnostics = "bad magic in raw-s21.h5";
        return false;
    }
    std::uint32_t ver = 0;
    std::uint32_t n_ch = 0, n_att = 0, n_ph = 0, n_f = 0;
    if (!readPod(f, ver) || ver != kVersion) {
        diagnostics = "unsupported raw-s21.h5 version";
        return false;
    }
    if (!readPod(f, n_ch) || !readPod(f, n_att) || !readPod(f, n_ph) || !readPod(f, n_f)) {
        diagnostics = "cannot read dims";
        return false;
    }
    if (n_ch == 0 || n_att == 0 || n_ph == 0 || n_f == 0) {
        diagnostics = "invalid zero dimension";
        return false;
    }

    std::vector<std::uint8_t> channels;
    std::vector<std::uint16_t> att_codes;
    std::vector<std::uint8_t> phase_codes;
    std::vector<std::uint64_t> frequency_hz;
    if (!readVec(f, channels, n_ch) || !readVec(f, att_codes, n_att)
        || !readVec(f, phase_codes, n_ph) || !readVec(f, frequency_hz, n_f)) {
        diagnostics = "cannot read axes";
        return false;
    }

    RawS21Meta meta;
    if (!readMeta(f, meta)) {
        diagnostics = "cannot read meta";
        return false;
    }

    const auto state_stride = computeStateStride(n_f);
    const auto ref_stride = computeRefStride(n_f);
    const auto states_offset = static_cast<std::uint64_t>(f.tellg());
    const std::size_t n_states = static_cast<std::size_t>(n_ch) * n_att * n_ph;
    const auto refs_offset = states_offset + state_stride * n_states;

    out.file_ = std::move(f);
    out.path_ = path;
    out.n_channel_ = n_ch;
    out.n_att_ = n_att;
    out.n_phase_ = n_ph;
    out.n_freq_ = n_f;
    out.channels_ = std::move(channels);
    out.att_codes_ = std::move(att_codes);
    out.phase_codes_ = std::move(phase_codes);
    out.frequency_hz_ = std::move(frequency_hz);
    out.meta_ = std::move(meta);
    out.states_offset_ = states_offset;
    out.refs_offset_ = refs_offset;
    out.state_stride_ = state_stride;
    out.ref_stride_ = ref_stride;
    return true;
}

std::size_t RawS21Store::flatIndex(std::size_t ch_i, std::size_t att_i, std::size_t ph_i) const
{
    return (ch_i * n_att_ + att_i) * n_phase_ + ph_i;
}

std::optional<std::size_t> RawS21Store::indexOf(std::uint8_t channel,
                                                std::uint16_t att_code,
                                                std::uint8_t phase_code) const
{
    std::size_t ch_i = static_cast<std::size_t>(-1);
    std::size_t att_i = static_cast<std::size_t>(-1);
    std::size_t ph_i = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < channels_.size(); ++i) {
        if (channels_[i] == channel) {
            ch_i = i;
            break;
        }
    }
    for (std::size_t i = 0; i < att_codes_.size(); ++i) {
        if (att_codes_[i] == att_code) {
            att_i = i;
            break;
        }
    }
    for (std::size_t i = 0; i < phase_codes_.size(); ++i) {
        if (phase_codes_[i] == phase_code) {
            ph_i = i;
            break;
        }
    }
    if (ch_i == static_cast<std::size_t>(-1) || att_i == static_cast<std::size_t>(-1)
        || ph_i == static_cast<std::size_t>(-1)) {
        return std::nullopt;
    }
    return flatIndex(ch_i, att_i, ph_i);
}

bool RawS21Store::seekState(std::size_t flat) const
{
    const auto off = static_cast<std::streamoff>(states_offset_ + flat * state_stride_);
    file_.seekg(off);
    file_.seekp(off);
    return static_cast<bool>(file_);
}

bool RawS21Store::seekRef(std::size_t flat) const
{
    const auto off = static_cast<std::streamoff>(refs_offset_ + flat * ref_stride_);
    file_.seekg(off);
    file_.seekp(off);
    return static_cast<bool>(file_);
}

bool RawS21Store::writeRecordAt(std::size_t flat,
                                const RawS21StateRecord& record,
                                std::string& diagnostics)
{
    if (record.s21.size() != n_freq_ || record.valid.size() != n_freq_) {
        diagnostics = "writeState: length mismatch vs n_freq";
        return false;
    }
    if (!seekState(flat)) {
        diagnostics = "writeState: seek failed";
        return false;
    }
    std::vector<double> re(n_freq_), im(n_freq_);
    for (std::uint32_t i = 0; i < n_freq_; ++i) {
        re[i] = record.s21[i].real();
        im[i] = record.s21[i].imag();
    }
    if (!writeVec(file_, re) || !writeVec(file_, im)) {
        diagnostics = "writeState: s21 write failed";
        return false;
    }
    if (!writePod(file_, record.temperature_c)) {
        diagnostics = "writeState: temp write failed";
        return false;
    }
    const std::uint8_t ov = record.overload ? 1 : 0;
    if (!writePod(file_, ov)) {
        diagnostics = "writeState: overload write failed";
        return false;
    }
    std::vector<std::uint8_t> valid(n_freq_);
    for (std::uint32_t i = 0; i < n_freq_; ++i) {
        valid[i] = record.valid[i] ? 1 : 0;
    }
    if (!writeVec(file_, valid)) {
        diagnostics = "writeState: valid write failed";
        return false;
    }
    if (!writePod(file_, record.attempt)) {
        diagnostics = "writeState: attempt write failed";
        return false;
    }
    const std::uint8_t done = record.completed ? 1 : 0;
    if (!writePod(file_, done)) {
        diagnostics = "writeState: completed write failed";
        return false;
    }
    file_.flush();
    return static_cast<bool>(file_);
}

bool RawS21Store::readRecordAt(std::size_t flat,
                               RawS21StateRecord& out,
                               std::string& diagnostics) const
{
    if (!seekState(flat)) {
        diagnostics = "readState: seek failed";
        return false;
    }
    std::vector<double> re, im;
    if (!readVec(file_, re, n_freq_) || !readVec(file_, im, n_freq_)) {
        diagnostics = "readState: s21 read failed";
        return false;
    }
    out.s21.resize(n_freq_);
    for (std::uint32_t i = 0; i < n_freq_; ++i) {
        out.s21[i] = {re[i], im[i]};
    }
    if (!readPod(file_, out.temperature_c)) {
        diagnostics = "readState: temp read failed";
        return false;
    }
    std::uint8_t ov = 0;
    if (!readPod(file_, ov)) {
        diagnostics = "readState: overload read failed";
        return false;
    }
    out.overload = ov != 0;
    std::vector<std::uint8_t> valid;
    if (!readVec(file_, valid, n_freq_)) {
        diagnostics = "readState: valid read failed";
        return false;
    }
    out.valid.resize(n_freq_);
    for (std::uint32_t i = 0; i < n_freq_; ++i) {
        out.valid[i] = valid[i] != 0 ? 1 : 0;
    }
    if (!readPod(file_, out.attempt)) {
        diagnostics = "readState: attempt read failed";
        return false;
    }
    std::uint8_t done = 0;
    if (!readPod(file_, done)) {
        diagnostics = "readState: completed read failed";
        return false;
    }
    out.completed = done != 0;
    return true;
}

bool RawS21Store::writeState(std::uint8_t channel,
                             std::uint16_t att_code,
                             std::uint8_t phase_code,
                             const RawS21StateRecord& record,
                             std::string& diagnostics)
{
    diagnostics.clear();
    const auto idx = indexOf(channel, att_code, phase_code);
    if (!idx) {
        diagnostics = "writeState: unknown (channel,att,phase)";
        return false;
    }
    RawS21StateRecord existing;
    if (!readRecordAt(*idx, existing, diagnostics)) {
        return false;
    }
    if (existing.completed) {
        diagnostics = "writeState: slot already completed (AT-05)";
        return false;
    }
    return writeRecordAt(*idx, record, diagnostics);
}

bool RawS21Store::readState(std::uint8_t channel,
                            std::uint16_t att_code,
                            std::uint8_t phase_code,
                            RawS21StateRecord& out,
                            std::string& diagnostics) const
{
    diagnostics.clear();
    const auto idx = indexOf(channel, att_code, phase_code);
    if (!idx) {
        diagnostics = "readState: unknown (channel,att,phase)";
        return false;
    }
    return readRecordAt(*idx, out, diagnostics);
}

bool RawS21Store::isCompleted(std::uint8_t channel,
                              std::uint16_t att_code,
                              std::uint8_t phase_code) const
{
    RawS21StateRecord rec;
    std::string diag;
    if (!readState(channel, att_code, phase_code, rec, diag)) {
        return false;
    }
    return rec.completed;
}

bool RawS21Store::clearCompleted(std::uint8_t channel,
                                 std::uint16_t att_code,
                                 std::uint8_t phase_code,
                                 std::string& diagnostics)
{
    diagnostics.clear();
    const auto idx = indexOf(channel, att_code, phase_code);
    if (!idx) {
        diagnostics = "clearCompleted: unknown (channel,att,phase)";
        return false;
    }
    RawS21StateRecord rec;
    if (!readRecordAt(*idx, rec, diagnostics)) {
        return false;
    }
    if (!rec.completed) {
        return true;
    }
    rec.completed = false;
    return writeRecordAt(*idx, rec, diagnostics);
}

std::size_t RawS21Store::completedCount() const
{
    std::size_t n = 0;
    const std::size_t total =
        static_cast<std::size_t>(n_channel_) * n_att_ * n_phase_;
    for (std::size_t i = 0; i < total; ++i) {
        RawS21StateRecord rec;
        std::string diag;
        if (readRecordAt(i, rec, diag) && rec.completed) {
            ++n;
        }
    }
    return n;
}

std::size_t RawS21Store::totalComplexSamplesCompleted() const
{
    return completedCount() * static_cast<std::size_t>(n_freq_);
}

bool RawS21Store::writeReference(std::uint8_t channel,
                                 std::uint32_t att_row,
                                 const std::vector<std::complex<double>>& s21,
                                 std::string& diagnostics)
{
    diagnostics.clear();
    if (att_row > n_att_) {
        diagnostics = "writeReference: att_row out of range";
        return false;
    }
    if (s21.size() != n_freq_) {
        diagnostics = "writeReference: length mismatch";
        return false;
    }
    std::size_t ch_i = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < channels_.size(); ++i) {
        if (channels_[i] == channel) {
            ch_i = i;
            break;
        }
    }
    if (ch_i == static_cast<std::size_t>(-1)) {
        diagnostics = "writeReference: unknown channel";
        return false;
    }
    const std::size_t flat = ch_i * (static_cast<std::size_t>(n_att_) + 1) + att_row;
    if (!seekRef(flat)) {
        diagnostics = "writeReference: seek failed";
        return false;
    }
    std::vector<double> re(n_freq_), im(n_freq_);
    for (std::uint32_t i = 0; i < n_freq_; ++i) {
        re[i] = s21[i].real();
        im[i] = s21[i].imag();
    }
    const std::uint8_t written = 1;
    if (!writeVec(file_, re) || !writeVec(file_, im) || !writePod(file_, written)) {
        diagnostics = "writeReference: write failed";
        return false;
    }
    file_.flush();
    return static_cast<bool>(file_);
}

bool RawS21Store::readReference(std::uint8_t channel,
                                std::uint32_t att_row,
                                std::vector<std::complex<double>>& s21,
                                std::string& diagnostics) const
{
    diagnostics.clear();
    if (att_row > n_att_) {
        diagnostics = "readReference: att_row out of range";
        return false;
    }
    std::size_t ch_i = static_cast<std::size_t>(-1);
    for (std::size_t i = 0; i < channels_.size(); ++i) {
        if (channels_[i] == channel) {
            ch_i = i;
            break;
        }
    }
    if (ch_i == static_cast<std::size_t>(-1)) {
        diagnostics = "readReference: unknown channel";
        return false;
    }
    const std::size_t flat = ch_i * (static_cast<std::size_t>(n_att_) + 1) + att_row;
    if (!seekRef(flat)) {
        diagnostics = "readReference: seek failed";
        return false;
    }
    std::vector<double> re, im;
    std::uint8_t written = 0;
    if (!readVec(file_, re, n_freq_) || !readVec(file_, im, n_freq_) || !readPod(file_, written)) {
        diagnostics = "readReference: read failed";
        return false;
    }
    if (!written) {
        diagnostics = "readReference: slot empty";
        return false;
    }
    s21.resize(n_freq_);
    for (std::uint32_t i = 0; i < n_freq_; ++i) {
        s21[i] = {re[i], im[i]};
    }
    return true;
}

}  // namespace afar
