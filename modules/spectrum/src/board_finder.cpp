#include "board_finder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#endif

namespace {

// Порт 9 (discard) — содержимое датаграммы никого не интересует, важен лишь
// сам факт отправки: он заставляет ядро разрешить ARP для адреса назначения.
constexpr uint16_t PROBE_PORT = 9;

std::string normalize_mac(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        if (c == ':' || c == '-' || c == ' ') continue;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out; // 12 шестнадцатеричных цифр без разделителей
}

bool valid_mac(const std::string& normalized) {
    if (normalized.size() != 12) return false;
    return std::all_of(normalized.begin(), normalized.end(),
                        [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

#ifdef _WIN32
// Только для таблицы Windows: там аппаратный адрес приходит массивом байт,
// тогда как /proc/net/arp отдаёт его уже текстом.
std::string mac_from_bytes(const unsigned char* b) {
    char buf[13];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
    return std::string(buf);
}
#endif

// Локальный адрес, с которого ушёл бы пакет наружу. connect() на UDP-сокете
// пакетов не шлёт, а лишь заставляет ядро выбрать маршрут и привязать сокет,
// после чего getsockname() возвращает нужный адрес. Работает одинаково на
// Linux и Windows, в отличие от разбора таблицы маршрутов.
bool local_ipv4(uint32_t& out_be) {
    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (fd < 0) return false;

    sockaddr_in probe{};
    probe.sin_family      = AF_INET;
    probe.sin_port        = htons(53);
    probe.sin_addr.s_addr = htonl(0x08080808u); // 8.8.8.8, пакеты не отправляются

    bool ok = false;
    if (::connect(fd, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
#ifdef _WIN32
        int len = sizeof(local);
#else
        socklen_t len = sizeof(local);
#endif
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0) {
            out_be = local.sin_addr.s_addr;
            ok     = true;
        }
    }
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
    return ok;
}

// Будим всю /24: маска шире здесь смысла не имеет — обход /16 занял бы минуты,
// а лабораторная сеть платы всегда /24.
void probe_subnet(uint32_t local_be) {
    int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, 0));
    if (fd < 0) return;

    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));

    uint32_t base = ntohl(local_be) & 0xFFFFFF00u;
    const char payload = 0;

    for (uint32_t host = 1; host <= 254; ++host) {
        sockaddr_in dst{};
        dst.sin_family      = AF_INET;
        dst.sin_port        = htons(PROBE_PORT);
        dst.sin_addr.s_addr = htonl(base | host);
        ::sendto(fd, &payload, sizeof(payload), 0,
                 reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }

#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

// Адрес, которому таблица соседей сейчас приписывает искомый MAC. Пустая
// строка — не найден. Неполные записи (без аппаратного адреса) пропускаем:
// именно они однажды подсунули скрипту чужой адрес во время опроса.
std::string ip_with_mac(const std::string& want_mac) {
#ifdef _WIN32
    ULONG size = 0;
    if (::GetIpNetTable(nullptr, &size, FALSE) != ERROR_INSUFFICIENT_BUFFER) return {};
    std::vector<char> buf(size);
    auto* table = reinterpret_cast<MIB_IPNETTABLE*>(buf.data());
    if (::GetIpNetTable(table, &size, FALSE) != NO_ERROR) return {};

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const MIB_IPNETROW& row = table->table[i];
        if (row.dwPhysAddrLen != 6) continue;
        if (row.dwType == MIB_IPNET_TYPE_INVALID) continue;
        if (mac_from_bytes(row.bPhysAddr) != want_mac) continue;

        in_addr a{};
        a.S_un.S_addr = row.dwAddr;
        char text[INET_ADDRSTRLEN]{};
        if (::inet_ntop(AF_INET, &a, text, sizeof(text))) return text;
    }
    return {};
#else
    std::FILE* f = std::fopen("/proc/net/arp", "r");
    if (!f) return {};

    char line[256];
    if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return {}; } // заголовок

    std::string result;
    while (std::fgets(line, sizeof(line), f)) {
        char ip[64]{}, hw[64]{}, mask[64]{}, dev[64]{};
        unsigned type = 0, flags = 0;
        // IP HW-type Flags HW-address Mask Device
        if (std::sscanf(line, "%63s %x %x %63s %63s %63s", ip, &type, &flags, hw, mask, dev) != 6)
            continue;
        if (flags == 0) continue; // неполная запись — аппаратный адрес недостоверен
        if (normalize_mac(hw) != want_mac) continue;
        result = ip;
        break;
    }
    std::fclose(f);
    return result;
#endif
}

} // namespace

BoardFindResult find_board_by_mac(const std::string& mac_text) {
    BoardFindResult r;

    const std::string want = normalize_mac(mac_text);
    if (!valid_mac(want)) {
        r.status = "неверный MAC (нужно 6 байт, например 42:7e:d7:4e:0f:77)";
        return r;
    }

    // Сначала таблица соседей: если плату уже трогали, ответ будет мгновенным.
    std::string ip = ip_with_mac(want);

    if (ip.empty()) {
        uint32_t local_be = 0;
        if (!local_ipv4(local_be)) {
            r.status = "не удалось определить локальный адрес — нет маршрута наружу?";
            return r;
        }
        probe_subnet(local_be);
        // Дать ядру время разослать ARP и дождаться ответов.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        ip = ip_with_mac(want);
    }

    if (ip.empty()) {
        r.status = "плата с таким MAC в подсети не найдена";
        return r;
    }

    r.found  = true;
    r.ip     = ip;
    r.status = "найдена: " + ip;
    return r;
}

void BoardFinderThread::run() {
    while (state_.running.load()) {
        if (!state_.board_find_requested.exchange(false)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        std::string mac;
        {
            std::lock_guard<std::mutex> lock(state_.board_mutex);
            mac = state_.board_mac;
            state_.board_status = "ищу...";
            state_.board_found_ip.clear();
        }
        state_.board_is_finding.store(true);

        BoardFindResult res = find_board_by_mac(mac);

        {
            std::lock_guard<std::mutex> lock(state_.board_mutex);
            state_.board_found_ip = res.found ? res.ip : std::string{};
            state_.board_status   = res.status;
        }
        state_.board_is_finding.store(false);
    }
}
