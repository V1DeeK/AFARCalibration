#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "C2220Vna.h"
#include "ScpiComTransport.h"
#include "ScpiSocketTransport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
using StubSocket = SOCKET;
static constexpr StubSocket kStubInvalid = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using StubSocket = int;
static constexpr StubSocket kStubInvalid = -1;
#endif

using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace {

#ifdef _WIN32
struct WinsockOnce {
    WinsockOnce()
    {
        WSADATA wsa{};
        REQUIRE(::WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
    }
    ~WinsockOnce() { ::WSACleanup(); }
};
void ensure_stub_winsock()
{
    static WinsockOnce once;
    (void)once;
}
void stub_close(StubSocket s)
{
    if (s != kStubInvalid) {
        ::closesocket(s);
    }
}
#else
void ensure_stub_winsock() {}
void stub_close(StubSocket s)
{
    if (s != kStubInvalid) {
        ::close(s);
    }
}
#endif

/// Минимальный SCPI TCP-стаб C2220 для AT-01 / драйвера.
class ScpiTcpStub {
public:
    std::string idn{"PLANAR,C2220,STUB001,1.0"};
    int directAccess{0};  // 0/1 ответ на SYST:REC:DIR:ACC?

    void start()
    {
        ensure_stub_winsock();
        stop();

        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(listen_ != kStubInvalid);

        int yes = 1;
#ifdef _WIN32
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                     sizeof(yes));
#else
        ::setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(0);
        REQUIRE(::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        REQUIRE(::getsockname(listen_, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
        port_ = ntohs(bound.sin_port);
        REQUIRE(::listen(listen_, 1) == 0);

        running_ = true;
        thread_ = std::thread([this] { serve(); });
    }

    void stop()
    {
        running_ = false;
        stub_close(listen_);
        listen_ = kStubInvalid;
        stub_close(client_);
        client_ = kStubInvalid;
        if (thread_.joinable()) {
            thread_.join();
        }
        port_ = 0;
    }

    ~ScpiTcpStub() { stop(); }

    std::uint16_t port() const noexcept { return port_; }

    std::vector<std::string> commands() const
    {
        std::lock_guard lock(mu_);
        return commands_;
    }

    bool saw_command_prefix(const std::string& prefix) const
    {
        std::lock_guard lock(mu_);
        for (const auto& c : commands_) {
            if (c.rfind(prefix, 0) == 0) {
                return true;
            }
        }
        return false;
    }

    bool saw_exact(const std::string& cmd) const
    {
        std::lock_guard lock(mu_);
        for (const auto& c : commands_) {
            if (c == cmd) {
                return true;
            }
        }
        return false;
    }

private:
    void record(std::string cmd)
    {
        std::lock_guard lock(mu_);
        commands_.push_back(std::move(cmd));
    }

    static bool send_all(StubSocket s, const std::string& data)
    {
        std::size_t off = 0;
        while (off < data.size()) {
#ifdef _WIN32
            const int n = ::send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#else
            const ssize_t n = ::send(s, data.data() + off, data.size() - off, 0);
#endif
            if (n <= 0) {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    static bool read_line(StubSocket s, std::string& line)
    {
        line.clear();
        char ch = 0;
        for (;;) {
#ifdef _WIN32
            const int n = ::recv(s, &ch, 1, 0);
#else
            const ssize_t n = ::recv(s, &ch, 1, 0);
#endif
            if (n <= 0) {
                return false;
            }
            if (ch == '\n') {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return true;
            }
            line.push_back(ch);
        }
    }

    void reply(StubSocket s, const std::string& body)
    {
        send_all(s, body + "\n");
    }

    void handle(const std::string& cmd, StubSocket s)
    {
        record(cmd);

        if (cmd == "*IDN?") {
            reply(s, idn);
            return;
        }
        if (cmd == "SYST:REC:DIR:ACC?") {
            reply(s, std::to_string(directAccess));
            return;
        }
        if (cmd == "*OPC?") {
            reply(s, "1");
            return;
        }
        if (cmd == "SYST:ERR?") {
            reply(s, "0,\"No error\"");
            return;
        }
        if (cmd == "CALC:DATA:SDAT?") {
            // 3 точки: (1,0), (0.5,0.5), (0,1)
            reply(s, "1,0,0.5,0.5,0,1");
            return;
        }
        if (cmd == "CALC:DATA:XAX?") {
            reply(s, "1000000000,1500000000,2000000000");
            return;
        }
        // Команды без ответа (configure / TRIG:SING) — молча OK.
        if (cmd.rfind("SENS:", 0) == 0 || cmd.rfind("SOUR:", 0) == 0
            || cmd.rfind("CALC:PAR:DEF", 0) == 0 || cmd == "TRIG:SING") {
            return;
        }
        // Неизвестный запрос с '?' — пустой ответ, чтобы не зависнуть.
        if (cmd.find('?') != std::string::npos) {
            reply(s, "");
        }
    }

    void serve()
    {
        while (running_) {
            StubSocket c = ::accept(listen_, nullptr, nullptr);
            if (c == kStubInvalid) {
                break;
            }
            client_ = c;
            std::string line;
            while (running_ && read_line(c, line)) {
                handle(line, c);
            }
            stub_close(c);
            client_ = kStubInvalid;
        }
    }

    StubSocket listen_{kStubInvalid};
    StubSocket client_{kStubInvalid};
    std::uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<std::string> commands_;
};

}  // namespace

TEST_CASE("AT-01 C2220Vna identify via TCP stub", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    transport.set_connect_timeout_ms(2000);
    C2220Vna vna(transport);

    REQUIRE_NOTHROW(vna.connect());
    const auto idn = vna.identify();
    REQUIRE_THAT(idn, ContainsSubstring("PLANAR"));
    REQUIRE_THAT(idn, ContainsSubstring("C2220"));
    REQUIRE(stub.saw_exact("*IDN?"));
    REQUIRE(stub.saw_exact("SYST:REC:DIR:ACC?"));
    REQUIRE_FALSE(stub.saw_command_prefix("SYST:REC:DIR:ACC "));
}

TEST_CASE("C2220Vna rejects foreign model", "[c2220]")
{
    ScpiTcpStub stub;
    stub.idn = "OTHERVENDOR,X9999,0000,0.0";
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    REQUIRE_NOTHROW(vna.connect());
    REQUIRE_THROWS_AS(vna.identify(), std::runtime_error);
}

TEST_CASE("direct access ON without allow flag is error and never sends ON", "[c2220]")
{
    ScpiTcpStub stub;
    stub.directAccess = 1;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna::Profile profile;
    profile.allow_direct_access = false;
    C2220Vna vna(transport, profile);

    REQUIRE_THROWS_AS(vna.connect(), std::runtime_error);
    REQUIRE(stub.saw_exact("SYST:REC:DIR:ACC?"));
    REQUIRE_FALSE(stub.saw_exact("SYST:REC:DIR:ACC ON"));
    REQUIRE_FALSE(stub.saw_exact("SYST:REC:DIR:ACC 1"));
    REQUIRE_FALSE(stub.saw_command_prefix("SYST:REC:DIR:ACC ON"));
}

TEST_CASE("direct access ON allowed by profile", "[c2220]")
{
    ScpiTcpStub stub;
    stub.directAccess = 1;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna::Profile profile;
    profile.allow_direct_access = true;
    C2220Vna vna(transport, profile);

    REQUIRE_NOTHROW(vna.connect());
    REQUIRE_THAT(vna.identify(), ContainsSubstring("C2220"));
    REQUIRE_FALSE(stub.saw_command_prefix("SYST:REC:DIR:ACC ON"));
}

TEST_CASE("C2220Vna configure and measure_s21 against stub", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();
    REQUIRE_THAT(vna.identify(), ContainsSubstring("C2220"));

    SweepConfig cfg{};
    cfg.f_start_hz = 1'000'000'000ULL;
    cfg.f_stop_hz = 2'000'000'000ULL;
    cfg.points = 3;
    cfg.power_dbm = -20.0;
    cfg.ifbw_hz = 1000;
    cfg.averages = 1;
    cfg.s_parameter = SParameter::S21;
    REQUIRE_NOTHROW(vna.configure(cfg));

    const auto sweep = vna.measure_s21();
    REQUIRE(sweep.frequency_hz.size() == 3);
    REQUIRE(sweep.s21.size() == 3);
    REQUIRE(sweep.s11.empty());
    REQUIRE(sweep.s12.empty());
    REQUIRE(sweep.s22.empty());
    REQUIRE(sweep.frequency_hz[0] == 1'000'000'000ULL);
    REQUIRE(sweep.frequency_hz[2] == 2'000'000'000ULL);
    REQUIRE(sweep.s21[0].real() == Approx(1.0));
    REQUIRE(sweep.s21[2].imag() == Approx(1.0));

    REQUIRE(stub.saw_command_prefix("SENS:FREQ:STAR "));
    REQUIRE(stub.saw_exact("CALC:PAR:DEF S21"));
    REQUIRE(stub.saw_exact("TRIG:SING"));
    REQUIRE(stub.saw_exact("*OPC?"));
    REQUIRE(stub.saw_exact("CALC:DATA:SDAT?"));
    REQUIRE(stub.saw_exact("CALC:DATA:XAX?"));

    const auto errs = vna.drain_errors();
    REQUIRE(errs.empty());
}

TEST_CASE("C2220Vna configure S11 fills s11 via measure_trace", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    SweepConfig cfg{};
    cfg.f_start_hz = 1'000'000'000ULL;
    cfg.f_stop_hz = 2'000'000'000ULL;
    cfg.points = 3;
    cfg.power_dbm = -20.0;
    cfg.ifbw_hz = 1000;
    cfg.averages = 1;
    cfg.s_parameter = SParameter::S11;
    REQUIRE_NOTHROW(vna.configure(cfg));

    const auto sweep = vna.measure_trace();
    REQUIRE(stub.saw_exact("CALC:PAR:DEF S11"));
    REQUIRE(sweep.frequency_hz.size() == 3);
    REQUIRE(sweep.s11.size() == 3);
    REQUIRE(sweep.s21.empty());
    REQUIRE(sweep.s12.empty());
    REQUIRE(sweep.s22.empty());
    REQUIRE(sweep.s11[0].real() == Approx(1.0));
    REQUIRE_FALSE(sweep.overload);

    REQUIRE_THROWS_AS(vna.measure_s21(), std::runtime_error);
}

TEST_CASE("ScpiComTransport compiles and rejects missing port", "[c2220][com]")
{
#ifdef _WIN32
    ScpiComTransport com("COM9");
    // Без петли/адаптера порт обычно отсутствует — ожидаем ошибку CreateFile, не краш.
    REQUIRE_THROWS_AS(com.connect(), std::runtime_error);
#else
    ScpiComTransport com("COM9");
    REQUIRE_THROWS_AS(com.connect(), std::runtime_error);
#endif
}
