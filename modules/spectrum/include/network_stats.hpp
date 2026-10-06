#pragma once

// PC end of the subband-statistics link: the board (FZTest/cpp/fzstats_send.cpp,
// a C++ port of FZTest/*.zig) reads the statistics the FPGA PL already computed
// and streams them here, so nothing on this side is recomputed from raw IQ.
// The wire structs themselves live in subband_wire.hpp, shared with the board.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "peleng.hpp" // SubbandStats
#include "subband_wire.hpp"

// TCP server side (runs on the PC / spectrum_analyzer process). Accepts one
// board connection at a time and decodes the block-framed stream above into
// SubbandStats, so the rest of the pipeline (SubbandEstimator, display) is
// identical whether the stats came from local IQ or the board over the wire.
class NetworkSubbandReceiver {
public:
    explicit NetworkSubbandReceiver(uint16_t port);
    ~NetworkSubbandReceiver();

    NetworkSubbandReceiver(const NetworkSubbandReceiver&)            = delete;
    NetworkSubbandReceiver& operator=(const NetworkSubbandReceiver&) = delete;

    uint16_t port() const { return port_; }
    bool is_connected() const { return client_fd_ >= 0; }

    // Non-blocking-ish: accepts a pending client if none is connected yet,
    // then tries to read exactly one full block within timeout_ms. Returns
    // false (with out_stats left untouched) if nothing completed in time —
    // callers should just retry on the next loop iteration. A read error or
    // EOF from an established client drops it and goes back to listening.
    bool poll_block(std::vector<SubbandStats>& out_stats, SubbandWireHeader& out_header, int timeout_ms);

private:
    void close_client();
    void accept_pending(int timeout_ms);
    // reads exactly len bytes, honoring the socket's receive timeout; false on EOF/error/timeout
    bool read_exact(void* buf, size_t len);

    uint16_t port_;
    int      listen_fd_ = -1;
    int      client_fd_ = -1;
};
