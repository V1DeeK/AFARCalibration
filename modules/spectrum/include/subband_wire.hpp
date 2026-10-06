#pragma once

// Wire format for already-accumulated subband statistics travelling from the
// Zynq PS (FZTest/cpp/fzstats_send.cpp, which reads the PL registers) to the
// analyzer on the PC. Fields mirror the "Структура записи подполосы" table,
// section 17 of ad9361_zynq7020_wideband_pl_algorithm.pdf.
//
// Shared verbatim by both sides, so it deliberately depends on nothing but
// <cstdint> — the board build has no FFTW, no ImGui, no project headers.
//
// Stream shape: [SubbandWireHeader][subband_count records], repeated once per
// completed ping-pong buffer. All integers little-endian (both ends are LE).

#include <cstdint>

#pragma pack(push, 1)
struct SubbandWireHeader {
    uint32_t magic;          // SUBBAND_WIRE_MAGIC
    uint16_t version;        // SUBBAND_WIRE_VERSION
    uint16_t subband_count;  // records following this header
    uint32_t fft_size;       // N_FFT
    uint32_t subband_size;   // B, bins per subband
    uint32_t accum_frames;   // M, FFT frames accumulated per block
    uint64_t sample_rate_hz;
    uint64_t center_freq_hz;
};

struct SubbandWireRecord {
    uint16_t subband_index;
    uint16_t flags;          // SUBBAND_FLAG_*
    uint64_t s_sum_pow;      // S_ΣΣ[p]
    uint64_t s_diff_pow;     // S_ΔΔ[p]
    int64_t  s_diff_sum_re;  // Re{S_ΔΣ[p]}
    int64_t  s_diff_sum_im;  // Im{S_ΔΣ[p]}
    uint64_t s11;
    uint64_t s22;
    int64_t  s12_re;
    int64_t  s12_im;
};
#pragma pack(pop)

static_assert(sizeof(SubbandWireHeader) == 36, "SubbandWireHeader must stay packed/stable across board and PC");
static_assert(sizeof(SubbandWireRecord) == 68, "SubbandWireRecord must stay packed/stable across board and PC");

// 'F','Z','S','T' read little-endian as a uint32_t.
constexpr uint32_t SUBBAND_WIRE_MAGIC   = 0x54535A46u;
constexpr uint16_t SUBBAND_WIRE_VERSION = 1;

constexpr uint16_t SUBBAND_FLAG_VALID    = 1u << 0;
constexpr uint16_t SUBBAND_FLAG_OVERFLOW = 1u << 1;
constexpr uint16_t SUBBAND_FLAG_CLIPPED  = 1u << 2;
