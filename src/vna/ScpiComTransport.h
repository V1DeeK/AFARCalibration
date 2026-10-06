#pragma once

#include "IScpiTransport.h"

#include <cstdint>
#include <string>

/// Windows COM SCPI-транспорт через Win32 API (CreateFile/ReadFile/WriteFile).
/// Без Qt SerialPort — чтобы тесты не тянули Qt. Вне `_WIN32` connect бросает.
class ScpiComTransport final : public IScpiTransport {
public:
    /// @param port_name Имя порта, напр. `"COM7"` или `"\\\\.\\COM10"`.
    explicit ScpiComTransport(std::string port_name, std::uint32_t baud_rate = 115200);
    ~ScpiComTransport() override;

    ScpiComTransport(const ScpiComTransport&) = delete;
    ScpiComTransport& operator=(const ScpiComTransport&) = delete;

    void set_io_timeout_ms(std::uint32_t timeout_ms) override;

    void connect() override;
    void disconnect() noexcept override;
    bool is_connected() const noexcept override;

    void write_line(const std::string& line) override;
    std::string read_line() override;
    void abort() noexcept override;

    const std::string& port_name() const noexcept { return portName_; }

private:
    void close_handle() noexcept;
#ifdef _WIN32
    void apply_timeouts();
#endif

    std::string portName_;
    std::uint32_t baudRate_{115200};
    std::uint32_t ioTimeoutMs_{30000};
#ifdef _WIN32
    void* handle_{nullptr};  // HANDLE, без windows.h в публичном заголовке
#endif
    std::string readBuf_;
    bool abortRequested_{false};
};
