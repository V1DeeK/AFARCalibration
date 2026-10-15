#include "board_finder.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <cerrno>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include <iio.h>

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

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
};
#endif

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

std::vector<uint32_t> local_ipv4_addresses() {
    std::vector<uint32_t> result;
#ifdef _WIN32
    ULONG size = 15'000;
    std::vector<unsigned char> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG rc = ::GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, adapters, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        rc = ::GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, adapters, &size);
    }
    if (rc == NO_ERROR) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                continue;
            for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
                if (!address->Address.lpSockaddr
                    || address->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr);
                result.push_back(ipv4->sin_addr.s_addr);
            }
        }
    }
#endif
    if (result.empty()) {
        uint32_t address = 0;
        if (local_ipv4(address)) result.push_back(address);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
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

// Адреса, которым таблица соседей сейчас приписывает искомый MAC. Пустой
// список — не найдено. Неполные записи (без аппаратного адреса) пропускаем:
// именно они однажды подсунули скрипту чужой адрес во время опроса.
std::vector<std::string> ips_with_mac(const std::string& want_mac) {
    std::vector<std::string> result;
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
        if (::inet_ntop(AF_INET, &a, text, sizeof(text))) result.emplace_back(text);
    }
#else
    std::FILE* f = std::fopen("/proc/net/arp", "r");
    if (!f) return {};

    char line[256];
    if (!std::fgets(line, sizeof(line), f)) { std::fclose(f); return {}; } // заголовок

    while (std::fgets(line, sizeof(line), f)) {
        char ip[64]{}, hw[64]{}, mask[64]{}, dev[64]{};
        unsigned type = 0, flags = 0;
        // IP HW-type Flags HW-address Mask Device
        if (std::sscanf(line, "%63s %x %x %63s %63s %63s", ip, &type, &flags, hw, mask, dev) != 6)
            continue;
        if (flags == 0) continue; // неполная запись — аппаратный адрес недостоверен
        if (normalize_mac(hw) != want_mac) continue;
        result.emplace_back(ip);
    }
    std::fclose(f);
#endif
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool iio_is_available(const std::string& ip) {
    const std::string uri = "ip:" + ip;
    iio_context* context = iio_create_context_from_uri(uri.c_str());
    if (!context) return false;
    iio_context_destroy(context);
    return true;
}

std::string trim_value(const char* value, std::size_t size) {
    if (!value) return {};
    std::string result(value, size);
    const auto nul = result.find('\0');
    if (nul != std::string::npos) result.resize(nul);
    while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back())))
        result.pop_back();
    return result;
}

std::string read_channel_attr(iio_channel* channel, const char* attr) {
    std::array<char, 65'536> buffer{};
    const auto size = iio_channel_attr_read(channel, attr, buffer.data(), buffer.size());
    return size > 0 ? trim_value(buffer.data(), static_cast<std::size_t>(size)) : std::string{};
}

std::string readable_frequency(const std::string& raw) {
    if (raw.empty()) return "—";
    try {
        const double hz = std::stod(raw);
        std::ostringstream out;
        out << raw << " \u0413\u0446 (" << std::fixed << std::setprecision(6)
            << (hz / 1.0e6) << " \u041c\u0413\u0446)";
        return out.str();
    } catch (...) {
        return raw;
    }
}

int append_device_attr(iio_device*, const char* attr, const char* value,
                       std::size_t size, void* data) {
    auto& out = *static_cast<std::ostringstream*>(data);
    out << "  " << attr << " = " << trim_value(value, size) << '\n';
    return 0;
}

int append_channel_attr(iio_channel*, const char* attr, const char* value,
                        std::size_t size, void* data) {
    return append_device_attr(nullptr, attr, value, size, data);
}

int append_buffer_attr(iio_device*, const char* attr, const char* value,
                       std::size_t size, void* data) {
    return append_device_attr(nullptr, attr, value, size, data);
}

} // namespace

BoardFindResult find_board_by_mac(const std::string& mac_text) {
    BoardFindResult r;

#ifdef _WIN32
    static WinsockInit winsock;
#endif

    const std::string want = normalize_mac(mac_text);
    if (!valid_mac(want)) {
        r.status = "неверный MAC (нужно 6 байт, например 42:7e:d7:4e:0f:77)";
        return r;
    }

    // Сначала проверяем известные адреса. Старая ARP-запись после смены DHCP
    // принимается только если на адресе действительно отвечает libiio.
    auto candidates = ips_with_mac(want);
    auto reachable = std::find_if(candidates.begin(), candidates.end(), iio_is_available);

    if (reachable == candidates.end()) {
        const auto addresses = local_ipv4_addresses();
        if (addresses.empty()) {
            r.status = "не удалось определить локальный адрес — нет маршрута наружу?";
            return r;
        }
        for (const auto address : addresses) probe_subnet(address);
        // Дать ядру время разослать ARP и дождаться ответов.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        candidates = ips_with_mac(want);
        reachable = std::find_if(candidates.begin(), candidates.end(), iio_is_available);
    }

    if (candidates.empty()) {
        r.status = "плата с таким MAC в подсети не найдена";
        return r;
    }

    r.found  = true;
    r.ip     = reachable != candidates.end() ? *reachable : candidates.front();
    r.status = reachable != candidates.end()
        ? "найдена: " + r.ip
        : "MAC найден: " + r.ip + ", но служба libiio не отвечает";
    return r;
}

BoardParametersResult read_board_parameters(const std::string& ip) {
    BoardParametersResult result;
    if (ip.empty()) {
        result.status = "IP платы не определён";
        return result;
    }

    const std::string uri = "ip:" + ip;
    using ContextPtr = std::unique_ptr<iio_context, decltype(&iio_context_destroy)>;
    errno = 0;
    ContextPtr context(iio_create_context_from_uri(uri.c_str()), &iio_context_destroy);
    if (!context) {
        // Причина от libiio/ОС: отказ в соединении означает, что на порту 30431 службы iiod нет, таймаут — адрес недоступен.
        const int reason = errno != 0 ? errno : 0;
        result.status = "libiio не подключился к " + uri + " (служба iiod, порт 30431)";
        if (reason != 0) {
            result.status += ": " + std::string(std::strerror(reason > 0 ? reason : -reason));
        }
        return result;
    }

    std::ostringstream out;
    out << "IP-адрес: " << ip << '\n';
    out << "IIO URI: " << uri << '\n';
    if (const char* description = iio_context_get_description(context.get()))
        out << "Плата / ОС: " << description << '\n';
    unsigned int major = 0, minor = 0;
    char tag[8]{};
    if (iio_context_get_version(context.get(), &major, &minor, tag) == 0)
        out << "Версия удалённого libiio: " << major << '.' << minor << " (" << tag << ")\n";

    auto* phy = iio_context_find_device(context.get(), "ad9361-phy");
    if (!phy) {
        result.status = "libiio доступен, но устройство ad9361-phy не найдено";
        result.text = out.str();
        return result;
    }

    auto* rxLo = iio_device_find_channel(phy, "altvoltage0", true);
    auto* rx1 = iio_device_find_channel(phy, "voltage0", false);
    auto* rx2 = iio_device_find_channel(phy, "voltage1", false);
    out << "\nОСНОВНЫЕ ПАРАМЕТРЫ (только чтение)\n";
    out << "Центральная частота RX: "
        << readable_frequency(rxLo ? read_channel_attr(rxLo, "frequency") : "") << '\n';
    for (const auto& [name, channel] : {std::pair{"RX1", rx1}, std::pair{"RX2", rx2}}) {
        out << name << ":\n";
        out << "  дискретизация: "
            << readable_frequency(channel ? read_channel_attr(channel, "sampling_frequency") : "") << '\n';
        out << "  полоса: "
            << readable_frequency(channel ? read_channel_attr(channel, "rf_bandwidth") : "") << '\n';
        out << "  режим усиления: " << (channel ? read_channel_attr(channel, "gain_control_mode") : "") << '\n';
        out << "  усиление: " << (channel ? read_channel_attr(channel, "hardwaregain") : "") << '\n';
        out << "  RSSI: " << (channel ? read_channel_attr(channel, "rssi") : "") << '\n';
        out << "  RF-порт: " << (channel ? read_channel_attr(channel, "rf_port_select") : "") << '\n';
    }

    out << "\nКОНТЕКСТ\n";
    for (unsigned int i = 0; i < iio_context_get_attrs_count(context.get()); ++i) {
        const char* name = nullptr;
        const char* value = nullptr;
        if (iio_context_get_attr(context.get(), i, &name, &value) == 0)
            out << "  " << (name ? name : "?") << " = " << (value ? value : "") << '\n';
    }

    out << "\nВСЕ СТАНДАРТНЫЕ ДОСТУПНЫЕ IIO-АТРИБУТЫ\n";
    unsigned int readErrors = 0;
    const auto countError = [&readErrors](auto code) {
        if (code < 0) ++readErrors;
    };
    for (unsigned int di = 0; di < iio_context_get_devices_count(context.get()); ++di) {
        auto* device = iio_context_get_device(context.get(), di);
        if (!device) continue;
        const char* id = iio_device_get_id(device);
        const char* name = iio_device_get_name(device);
        out << "\n[Устройство " << (id ? id : "?");
        if (name) out << ": " << name;
        out << "]\n";
        countError(iio_device_attr_read_all(device, append_device_attr, &out));
        countError(iio_device_buffer_attr_read_all(device, append_buffer_attr, &out));
        if (iio_device_get_debug_attrs_count(device) > 0) {
            out << " Отладочные атрибуты (только чтение):\n";
            countError(iio_device_debug_attr_read_all(device, append_device_attr, &out));
        }
        for (unsigned int ci = 0; ci < iio_device_get_channels_count(device); ++ci) {
            auto* channel = iio_device_get_channel(device, ci);
            if (!channel || iio_channel_get_attrs_count(channel) == 0) continue;
            out << " Канал " << (iio_channel_get_id(channel) ? iio_channel_get_id(channel) : "?")
                << (iio_channel_is_output(channel) ? " (выход)" : " (вход)") << ":\n";
            countError(iio_channel_attr_read_all(channel, append_channel_attr, &out));
        }
    }

    if (readErrors > 0)
        out << "\nПредупреждение: не удалось прочитать групп атрибутов: " << readErrors << '\n';

    result.ok = true;
    result.text = out.str();
    result.status = readErrors == 0
        ? "параметры платы прочитаны без изменения настроек"
        : "основные параметры прочитаны; часть подробных IIO-атрибутов недоступна";
    return result;
}

void BoardFinderThread::run() {
    while (state_.running.load()) {
        if (!state_.board_find_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // Сначала поднимаем busy, затем снимаем request: UI не увидит ложное
        // завершение между двумя атомарными флагами.
        state_.board_is_finding.store(true);
        if (!state_.board_find_requested.exchange(false)) {
            state_.board_is_finding.store(false);
            continue;
        }

        std::string mac;
        std::string knownIp;
        const bool readParameters = state_.board_read_parameters_requested.exchange(false);
        {
            std::lock_guard<std::mutex> lock(state_.board_mutex);
            mac = state_.board_mac;
            knownIp = state_.board_known_ip;
            state_.board_status = "ищу...";
            state_.board_found_ip.clear();
            if (readParameters) state_.board_parameters.clear();
        }
        BoardFindResult res;
        if (!knownIp.empty()) {
            // Плата уже на связи по PS: её адрес известен, искать по MAC не нужно.
            res.found = true;
            res.ip = knownIp;
            res.status = "плата на связи по PS, адрес " + knownIp;
        } else {
            res = find_board_by_mac(mac);
        }

        BoardParametersResult parameters;
        if (res.found && readParameters) {
            {
                std::lock_guard<std::mutex> lock(state_.board_mutex);
                state_.board_found_ip = res.ip;
                state_.board_status = "IP найден, читаю параметры libiio...";
            }
            parameters = read_board_parameters(res.ip);
            res.status = parameters.status;
            // Адрес PS не обязан быть адресом libiio: пробуем плату по MAC — там, где служба libiio отвечает.
            if (!parameters.ok && !knownIp.empty() && !mac.empty()) {
                const std::string psAddress = res.ip;
                const BoardFindResult alt = find_board_by_mac(mac);
                if (alt.found && alt.ip != psAddress) {
                    BoardParametersResult second = read_board_parameters(alt.ip);
                    if (second.ok) {
                        parameters = std::move(second);
                        res = alt;
                        res.status = "libiio не отвечает на адресе PS " + psAddress +
                                     "; плата с MAC " + mac + " найдена по адресу " + alt.ip + ". " + parameters.status;
                    }
                } else if (!alt.found) {
                    res.status += "; плата с MAC " + mac + " по другому адресу не найдена";
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(state_.board_mutex);
            state_.board_found_ip = res.found ? res.ip : std::string{};
            state_.board_status   = res.status;
            if (readParameters) state_.board_parameters = std::move(parameters.text);
        }
        state_.board_is_finding.store(false);
    }
}
