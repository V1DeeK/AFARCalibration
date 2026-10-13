#include "network_stats.hpp"

#include <cstring>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t   = int;
using ssize_t_sock = int;
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
using ssize_t_sock = ssize_t;
#define CLOSE_SOCKET(fd) ::close(fd)
#endif

namespace {

#ifdef _WIN32
// libiio's Windows backend also calls WSAStartup; the refcount makes repeated
// init safe, and we never call WSACleanup so no other user gets torn down.
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
};
#endif

// once a block has started arriving, how long the rest of it may take before
// the link counts as dead
constexpr int BLOCK_TIMEOUT_MS = 2000;

void set_recv_timeout(int fd, int timeout_ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

} // namespace

NetworkSubbandReceiver::NetworkSubbandReceiver(uint16_t port) : port_(port) {
#ifdef _WIN32
    static WinsockInit winsock_init;
#endif

    listen_fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) {
        std::cerr << "[netstats] socket() failed\n";
        return;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port_);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_fd_, 1) != 0) {
        std::cerr << "[netstats] bind/listen on port " << port_ << " failed\n";
        CLOSE_SOCKET(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    std::cout << "[netstats] listening for board statistics on TCP port " << port_ << "\n";
}

NetworkSubbandReceiver::~NetworkSubbandReceiver() {
    close_client();
    if (listen_fd_ >= 0) CLOSE_SOCKET(listen_fd_);
}

void NetworkSubbandReceiver::close_client() {
    if (client_fd_ < 0) return;
    CLOSE_SOCKET(client_fd_);
    client_fd_ = -1;
}

void NetworkSubbandReceiver::accept_pending(int timeout_ms) {
    if (listen_fd_ < 0) return;

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<unsigned>(listen_fd_), &rfds);

    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(listen_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0) return;

    sockaddr_in peer{};
    socklen_t   peer_len = sizeof(peer);
    int fd = static_cast<int>(accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len));
    if (fd < 0) return;

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));

    char ip[INET_ADDRSTRLEN] = { 0 };
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    std::cout << "[netstats] board connected from " << ip << "\n";

    client_fd_ = fd;
}

bool NetworkSubbandReceiver::read_exact(void* buf, size_t len) {
    auto*  dst  = static_cast<char*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t_sock n = recv(client_fd_, dst + done, static_cast<int>(len - done), 0);
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool NetworkSubbandReceiver::poll_block(std::vector<SubbandStats>& out_stats,
                                         SubbandWireHeader& out_header,
                                         int timeout_ms) {
    if (client_fd_ < 0) {
        accept_pending(timeout_ms);
        if (client_fd_ < 0) return false;
    }

    // Wait for the block to *start* within the caller's timeout, but never
    // return mid-block: consuming half a header and coming back later would
    // desync the stream. Once the first byte is here, the rest gets a generous
    // window, and failing that window means the link is dead, not just idle.
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<unsigned>(client_fd_), &rfds);
    timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(client_fd_ + 1, &rfds, nullptr, nullptr, &tv) <= 0) return false;

    set_recv_timeout(client_fd_, BLOCK_TIMEOUT_MS);

    SubbandWireHeader header{};
    if (!read_exact(&header, sizeof(header))) {
        std::cout << "[netstats] board disconnected\n";
        close_client();
        return false;
    }

    if (header.magic != SUBBAND_WIRE_MAGIC || header.version != SUBBAND_WIRE_VERSION) {
        std::cerr << "[netstats] bad header (magic=0x" << std::hex << header.magic << std::dec
                   << ", version=" << header.version << ") — dropping connection\n";
        close_client();
        return false;
    }

    std::vector<SubbandWireRecord> records(header.subband_count);
    if (header.subband_count > 0 &&
        !read_exact(records.data(), records.size() * sizeof(SubbandWireRecord))) {
        std::cerr << "[netstats] truncated block — dropping connection\n";
        close_client();
        return false;
    }

    out_stats.assign(header.subband_count, SubbandStats{});
    for (const SubbandWireRecord& rec : records) {
        if (rec.subband_index >= header.subband_count) continue;
        SubbandStats& s = out_stats[rec.subband_index];
        s.sum_pow      = static_cast<double>(rec.s_sum_pow);
        s.diff_pow     = static_cast<double>(rec.s_diff_pow);
        s.diff_sum_re  = static_cast<double>(rec.s_diff_sum_re);
        s.diff_sum_im  = static_cast<double>(rec.s_diff_sum_im);
        s.s11          = static_cast<double>(rec.s11);
        s.s22          = static_cast<double>(rec.s22);
        s.s12_re       = static_cast<double>(rec.s12_re);
        s.s12_im       = static_cast<double>(rec.s12_im);
    }

    out_header = header;
    return true;
}
