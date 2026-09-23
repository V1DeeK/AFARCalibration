#include "ScpiComTransport.h"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace {

std::string ensure_newline(std::string line)
{
    if (line.empty() || line.back() != '\n') {
        line.push_back('\n');
    }
    return line;
}

#ifdef _WIN32
std::string win32_error_message(DWORD code, const char* what)
{
    return std::string(what) + ": Win32 " + std::to_string(code);
}

std::string normalize_com_path(std::string name)
{
    // CreateFile требует \\.\COMn для номеров >= 10; для единообразия — всегда.
    if (name.rfind("\\\\.\\", 0) == 0) {
        return name;
    }
    return "\\\\.\\" + name;
}
#endif

}  // namespace

ScpiComTransport::ScpiComTransport(std::string port_name, std::uint32_t baud_rate)
    : portName_(std::move(port_name))
    , baudRate_(baud_rate == 0 ? 115200u : baud_rate)
{
}

ScpiComTransport::~ScpiComTransport()
{
    disconnect();
}

void ScpiComTransport::set_io_timeout_ms(std::uint32_t timeout_ms)
{
    ioTimeoutMs_ = timeout_ms == 0 ? 1u : timeout_ms;
#ifdef _WIN32
    if (is_connected()) {
        apply_timeouts();
    }
#endif
}

bool ScpiComTransport::is_connected() const noexcept
{
#ifdef _WIN32
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
#else
    return false;
#endif
}

void ScpiComTransport::close_handle() noexcept
{
#ifdef _WIN32
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(static_cast<HANDLE>(handle_));
    }
    handle_ = nullptr;
#endif
}

void ScpiComTransport::disconnect() noexcept
{
    close_handle();
    readBuf_.clear();
}

void ScpiComTransport::abort() noexcept
{
    abortRequested_ = true;
    close_handle();
}

#ifdef _WIN32
void ScpiComTransport::apply_timeouts()
{
    if (!is_connected()) {
        return;
    }
    COMMTIMEOUTS timeouts{};
    // Читать пока не придут данные или не истечёт total timeout на операцию.
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant = ioTimeoutMs_;
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = ioTimeoutMs_;
    if (!::SetCommTimeouts(static_cast<HANDLE>(handle_), &timeouts)) {
        throw std::runtime_error(
            win32_error_message(::GetLastError(), "ScpiComTransport: SetCommTimeouts"));
    }
}
#endif

void ScpiComTransport::connect()
{
#ifndef _WIN32
    throw std::runtime_error("ScpiComTransport: available only on Windows");
#else
    abortRequested_ = false;
    disconnect();

    const std::string path = normalize_com_path(portName_);
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        throw std::runtime_error(
            win32_error_message(::GetLastError(),
                                ("ScpiComTransport: CreateFile " + path).c_str()));
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(h, &dcb)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        throw std::runtime_error(win32_error_message(err, "ScpiComTransport: GetCommState"));
    }
    dcb.BaudRate = baudRate_;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    if (!::SetCommState(h, &dcb)) {
        const DWORD err = ::GetLastError();
        ::CloseHandle(h);
        throw std::runtime_error(win32_error_message(err, "ScpiComTransport: SetCommState"));
    }

    handle_ = h;
    try {
        apply_timeouts();
        ::PurgeComm(static_cast<HANDLE>(handle_),
                    PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);
    } catch (...) {
        close_handle();
        throw;
    }
    readBuf_.clear();
#endif
}

void ScpiComTransport::write_line(const std::string& line)
{
#ifndef _WIN32
    (void)line;
    throw std::runtime_error("ScpiComTransport: available only on Windows");
#else
    if (!is_connected()) {
        throw std::runtime_error("ScpiComTransport: write without connect");
    }
    if (abortRequested_) {
        throw std::runtime_error("ScpiComTransport: aborted");
    }

    const std::string payload = ensure_newline(line);
    DWORD written = 0;
    if (!::WriteFile(static_cast<HANDLE>(handle_), payload.data(),
                     static_cast<DWORD>(payload.size()), &written, nullptr)
        || written != payload.size()) {
        throw std::runtime_error(
            win32_error_message(::GetLastError(), "ScpiComTransport: WriteFile"));
    }
#endif
}

std::string ScpiComTransport::read_line()
{
#ifndef _WIN32
    throw std::runtime_error("ScpiComTransport: available only on Windows");
#else
    if (!is_connected()) {
        throw std::runtime_error("ScpiComTransport: read without connect");
    }

    while (true) {
        if (abortRequested_) {
            throw std::runtime_error("ScpiComTransport: aborted");
        }

        const auto pos = readBuf_.find('\n');
        if (pos != std::string::npos) {
            std::string line = readBuf_.substr(0, pos);
            readBuf_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }

        char chunk[256];
        DWORD got = 0;
        if (!::ReadFile(static_cast<HANDLE>(handle_), chunk, sizeof(chunk), &got, nullptr)) {
            throw std::runtime_error(
                win32_error_message(::GetLastError(), "ScpiComTransport: ReadFile"));
        }
        if (got == 0) {
            throw std::runtime_error("ScpiComTransport: read timeout");
        }
        readBuf_.append(chunk, static_cast<std::size_t>(got));
    }
#endif
}
