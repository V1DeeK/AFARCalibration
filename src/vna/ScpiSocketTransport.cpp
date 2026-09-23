#include "ScpiSocketTransport.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

namespace {

#ifdef _WIN32
class WinsockLifetime {
public:
    WinsockLifetime()
    {
        WSADATA wsa{};
        const int rc = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) {
            throw std::runtime_error("ScpiSocketTransport: WSAStartup failed: "
                                     + std::to_string(rc));
        }
    }
    ~WinsockLifetime() { ::WSACleanup(); }
};

void ensure_winsock()
{
    static WinsockLifetime guard;
    (void)guard;
}

std::string last_socket_error(const char* what)
{
    return std::string(what) + ": WSA " + std::to_string(::WSAGetLastError());
}
#else
void ensure_winsock() {}

std::string last_socket_error(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}
#endif

std::string ensure_newline(std::string line)
{
    if (line.empty() || line.back() != '\n') {
        line.push_back('\n');
    }
    return line;
}

}  // namespace

ScpiSocketTransport::ScpiSocketTransport(std::string host, std::uint16_t port)
    : host_(std::move(host))
    , port_(port)
{
}

ScpiSocketTransport::~ScpiSocketTransport()
{
    disconnect();
}

void ScpiSocketTransport::set_connect_timeout_ms(std::uint32_t timeout_ms) noexcept
{
    connectTimeoutMs_ = timeout_ms == 0 ? 1u : timeout_ms;
}

void ScpiSocketTransport::set_io_timeout_ms(std::uint32_t timeout_ms)
{
    ioTimeoutMs_ = timeout_ms == 0 ? 1u : timeout_ms;
    if (is_connected()) {
        apply_recv_timeout();
    }
}

bool ScpiSocketTransport::is_connected() const noexcept
{
    return sock_ != kInvalidSocket;
}

void ScpiSocketTransport::close_socket() noexcept
{
    if (sock_ == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(sock_), SD_BOTH);
    ::closesocket(static_cast<SOCKET>(sock_));
#else
    ::shutdown(static_cast<int>(sock_), SHUT_RDWR);
    ::close(static_cast<int>(sock_));
#endif
    sock_ = kInvalidSocket;
}

void ScpiSocketTransport::disconnect() noexcept
{
    close_socket();
    readBuf_.clear();
}

void ScpiSocketTransport::abort() noexcept
{
    abortRequested_ = true;
    close_socket();
}

void ScpiSocketTransport::apply_recv_timeout()
{
    if (!is_connected()) {
        return;
    }
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(ioTimeoutMs_);
    if (::setsockopt(static_cast<SOCKET>(sock_), SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&ms), sizeof(ms))
        != 0) {
        throw std::runtime_error(last_socket_error("ScpiSocketTransport: SO_RCVTIMEO"));
    }
    if (::setsockopt(static_cast<SOCKET>(sock_), SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&ms), sizeof(ms))
        != 0) {
        throw std::runtime_error(last_socket_error("ScpiSocketTransport: SO_SNDTIMEO"));
    }
#else
    timeval tv{};
    tv.tv_sec = static_cast<time_t>(ioTimeoutMs_ / 1000u);
    tv.tv_usec = static_cast<suseconds_t>((ioTimeoutMs_ % 1000u) * 1000u);
    if (::setsockopt(static_cast<int>(sock_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        throw std::runtime_error(last_socket_error("ScpiSocketTransport: SO_RCVTIMEO"));
    }
    if (::setsockopt(static_cast<int>(sock_), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        throw std::runtime_error(last_socket_error("ScpiSocketTransport: SO_SNDTIMEO"));
    }
#endif
}

void ScpiSocketTransport::connect()
{
    ensure_winsock();
    abortRequested_ = false;
    disconnect();

#ifdef _WIN32
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port_);
    const int gai = ::getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &result);
    if (gai != 0 || result == nullptr) {
        throw std::runtime_error("ScpiSocketTransport: getaddrinfo failed for " + host_
                                 + ":" + portStr);
    }

    SOCKET candidate = INVALID_SOCKET;
    std::string lastErr = "ScpiSocketTransport: connect failed";
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        candidate = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (candidate == INVALID_SOCKET) {
            lastErr = last_socket_error("ScpiSocketTransport: socket");
            continue;
        }

        u_long nonblock = 1;
        if (::ioctlsocket(candidate, FIONBIO, &nonblock) != 0) {
            lastErr = last_socket_error("ScpiSocketTransport: FIONBIO");
            ::closesocket(candidate);
            candidate = INVALID_SOCKET;
            continue;
        }

        const int cr = ::connect(candidate, rp->ai_addr, static_cast<int>(rp->ai_addrlen));
        if (cr == 0) {
            break;
        }
        const int wsa = ::WSAGetLastError();
        if (wsa != WSAEWOULDBLOCK && wsa != WSAEINPROGRESS) {
            lastErr = last_socket_error("ScpiSocketTransport: connect");
            ::closesocket(candidate);
            candidate = INVALID_SOCKET;
            continue;
        }

        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(candidate, &wset);
        timeval tv{};
        tv.tv_sec = static_cast<long>(connectTimeoutMs_ / 1000u);
        tv.tv_usec = static_cast<long>((connectTimeoutMs_ % 1000u) * 1000u);
        const int sel = ::select(0, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) {
            lastErr = (sel == 0) ? "ScpiSocketTransport: connect timeout"
                                 : last_socket_error("ScpiSocketTransport: select");
            ::closesocket(candidate);
            candidate = INVALID_SOCKET;
            continue;
        }

        int so_error = 0;
        int len = sizeof(so_error);
        if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len)
                != 0
            || so_error != 0) {
            lastErr = "ScpiSocketTransport: connect SO_ERROR " + std::to_string(so_error);
            ::closesocket(candidate);
            candidate = INVALID_SOCKET;
            continue;
        }
        break;
    }
    ::freeaddrinfo(result);

    if (candidate == INVALID_SOCKET) {
        throw std::runtime_error(lastErr);
    }

    u_long blocking = 0;
    if (::ioctlsocket(candidate, FIONBIO, &blocking) != 0) {
        ::closesocket(candidate);
        throw std::runtime_error(last_socket_error("ScpiSocketTransport: set blocking"));
    }

    sock_ = static_cast<SocketHandle>(candidate);
#else
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string portStr = std::to_string(port_);
    const int gai = ::getaddrinfo(host_.c_str(), portStr.c_str(), &hints, &result);
    if (gai != 0 || result == nullptr) {
        throw std::runtime_error("ScpiSocketTransport: getaddrinfo failed for " + host_
                                 + ":" + portStr);
    }

    int candidate = -1;
    std::string lastErr = "ScpiSocketTransport: connect failed";
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        candidate = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (candidate < 0) {
            lastErr = last_socket_error("ScpiSocketTransport: socket");
            continue;
        }

        const int flags = ::fcntl(candidate, F_GETFL, 0);
        if (flags < 0 || ::fcntl(candidate, F_SETFL, flags | O_NONBLOCK) < 0) {
            lastErr = last_socket_error("ScpiSocketTransport: fcntl");
            ::close(candidate);
            candidate = -1;
            continue;
        }

        const int cr = ::connect(candidate, rp->ai_addr, rp->ai_addrlen);
        if (cr == 0) {
            break;
        }
        if (errno != EINPROGRESS) {
            lastErr = last_socket_error("ScpiSocketTransport: connect");
            ::close(candidate);
            candidate = -1;
            continue;
        }

        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(candidate, &wset);
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(connectTimeoutMs_ / 1000u);
        tv.tv_usec = static_cast<suseconds_t>((connectTimeoutMs_ % 1000u) * 1000u);
        const int sel = ::select(candidate + 1, nullptr, &wset, nullptr, &tv);
        if (sel <= 0) {
            lastErr = (sel == 0) ? "ScpiSocketTransport: connect timeout"
                                 : last_socket_error("ScpiSocketTransport: select");
            ::close(candidate);
            candidate = -1;
            continue;
        }

        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(candidate, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0) {
            lastErr = "ScpiSocketTransport: connect SO_ERROR " + std::to_string(so_error);
            ::close(candidate);
            candidate = -1;
            continue;
        }
        break;
    }
    ::freeaddrinfo(result);

    if (candidate < 0) {
        throw std::runtime_error(lastErr);
    }

    const int flags = ::fcntl(candidate, F_GETFL, 0);
    if (flags < 0 || ::fcntl(candidate, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        ::close(candidate);
        throw std::runtime_error(last_socket_error("ScpiSocketTransport: set blocking"));
    }

    sock_ = candidate;
#endif

    try {
        apply_recv_timeout();
    } catch (...) {
        close_socket();
        throw;
    }
    readBuf_.clear();
}

void ScpiSocketTransport::write_line(const std::string& line)
{
    if (!is_connected()) {
        throw std::runtime_error("ScpiSocketTransport: write without connect");
    }
    if (abortRequested_) {
        throw std::runtime_error("ScpiSocketTransport: aborted");
    }

    const std::string payload = ensure_newline(line);
    std::size_t sent = 0;
    while (sent < payload.size()) {
#ifdef _WIN32
        const int n = ::send(static_cast<SOCKET>(sock_), payload.data() + sent,
                             static_cast<int>(payload.size() - sent), 0);
#else
        const ssize_t n = ::send(static_cast<int>(sock_), payload.data() + sent,
                                 payload.size() - sent, 0);
#endif
        if (n <= 0) {
            throw std::runtime_error(last_socket_error("ScpiSocketTransport: send"));
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::string ScpiSocketTransport::read_line()
{
    if (!is_connected()) {
        throw std::runtime_error("ScpiSocketTransport: read without connect");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(ioTimeoutMs_);

    while (true) {
        if (abortRequested_) {
            throw std::runtime_error("ScpiSocketTransport: aborted");
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

        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("ScpiSocketTransport: read timeout");
        }

        char chunk[512];
#ifdef _WIN32
        const int n = ::recv(static_cast<SOCKET>(sock_), chunk, sizeof(chunk), 0);
        if (n == 0) {
            throw std::runtime_error("ScpiSocketTransport: peer closed");
        }
        if (n < 0) {
            const int err = ::WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("ScpiSocketTransport: read timeout");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            throw std::runtime_error(last_socket_error("ScpiSocketTransport: recv"));
        }
#else
        const ssize_t n = ::recv(static_cast<int>(sock_), chunk, sizeof(chunk), 0);
        if (n == 0) {
            throw std::runtime_error("ScpiSocketTransport: peer closed");
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    throw std::runtime_error("ScpiSocketTransport: read timeout");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            throw std::runtime_error(last_socket_error("ScpiSocketTransport: recv"));
        }
#endif
        readBuf_.append(chunk, static_cast<std::size_t>(n));
    }
}
