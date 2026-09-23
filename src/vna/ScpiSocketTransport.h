#pragma once

#include "IScpiTransport.h"

#include <cstdint>
#include <string>

/// TCP/IP Socket SCPI-транспорт (контракт vna-c2220-scpi § 2).
/// На Windows — Winsock2; без Qt.
class ScpiSocketTransport final : public IScpiTransport {
public:
    ScpiSocketTransport(std::string host, std::uint16_t port);
    ~ScpiSocketTransport() override;

    ScpiSocketTransport(const ScpiSocketTransport&) = delete;
    ScpiSocketTransport& operator=(const ScpiSocketTransport&) = delete;

    void set_connect_timeout_ms(std::uint32_t timeout_ms) noexcept;
    void set_io_timeout_ms(std::uint32_t timeout_ms) override;

    void connect() override;
    void disconnect() noexcept override;
    bool is_connected() const noexcept override;

    void write_line(const std::string& line) override;
    std::string read_line() override;
    void abort() noexcept override;

    std::uint16_t port() const noexcept { return port_; }
    const std::string& host() const noexcept { return host_; }

private:
    void close_socket() noexcept;
    void apply_recv_timeout();

    std::string host_;
    std::uint16_t port_{};
    std::uint32_t connectTimeoutMs_{3000};
    std::uint32_t ioTimeoutMs_{30000};
#ifdef _WIN32
    using SocketHandle = std::uintptr_t;
    static constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~std::uintptr_t{0});
#else
    using SocketHandle = int;
    static constexpr SocketHandle kInvalidSocket = -1;
#endif
    SocketHandle sock_{kInvalidSocket};
    std::string readBuf_;
    bool abortRequested_{false};
};
