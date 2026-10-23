#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "C2220Vna.h"
#include "ScpiSocketTransport.h"

#include <algorithm>
#include <array>
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
void stub_shutdown(StubSocket s)
{
    if (s != kStubInvalid) {
        ::shutdown(s, SD_BOTH);
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
void stub_shutdown(StubSocket s)
{
    if (s != kStubInvalid) {
        ::shutdown(s, SHUT_RDWR);
    }
}
#endif

/// Минимальный SCPI TCP-стаб C2220 для AT-01 / драйвера.
class ScpiTcpStub {
public:
    std::string idn{"PLANAR,C2220,STUB001,1.0"};
    int directAccess{0};  // 0/1 ответ на SYST:REC:DIR:ACC?
    std::atomic<int> calibrationSubclass{1};
    std::atomic<int> triggerSourceBus{1};
    std::atomic<int> continuous{0};
    int nativeTraceCount{1};
    std::array<std::string, 6> nativeParameters{"S11", "S21", "S12", "S22", "S11", "S22"};
    std::array<std::string, 6> nativeFormats{"MLOG", "MLOG", "MLOG", "MLOG", "SWR", "SWR"};
    std::array<int, 6> nativeMarkerCounts{};
    int nativeLayout{1};
    std::string stateSaveType{"CST"};
    int rfOutput{1};
    int correction{1};

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
        if (thread_.joinable()) {
            // На Linux/macOS close() не будит поток в accept()/recv():
            // рвём клиента shutdown() и будим accept() пустым подключением.
            stub_shutdown(client_);
            wake_accept();
            thread_.join();
        }
        stub_close(listen_);
        listen_ = kStubInvalid;
        client_ = kStubInvalid;
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
        if (cmd == "MMEM:STOR:STYP?") {
            reply(s, stateSaveType);
            return;
        }
        if (cmd.rfind("MMEM:STOR:STYP ", 0) == 0) {
            stateSaveType = cmd.substr(15);
            return;
        }
        if (cmd.rfind("MMEM:STOR:STAT \"", 0) == 0) return;
        if (cmd == "OUTP?") { reply(s, std::to_string(rfOutput)); return; }
        if (cmd == "OUTP ON" || cmd == "OUTP OFF") {
            rfOutput = cmd == "OUTP ON";
            return;
        }
        if (cmd == "SENS:CORR:STAT ON" || cmd == "SENS:CORR:STAT OFF") {
            correction = cmd == "SENS:CORR:STAT ON";
            return;
        }
        if (cmd.rfind("MMEM:LOAD:STAT \"", 0) == 0) {
            nativeTraceCount = 4;
            nativeLayout = 7;
            return;
        }
        if (cmd == "CALC1:PAR:COUN?") {
            reply(s, std::to_string(nativeTraceCount));
            return;
        }
        if (cmd.rfind("CALC1:PAR:COUN ", 0) == 0) {
            nativeTraceCount = std::stoi(cmd.substr(15));
            return;
        }
        if (cmd == "DISP:WIND1:SPL?") {
            reply(s, std::to_string(nativeLayout));
            return;
        }
        if (cmd.rfind("DISP:WIND1:SPL ", 0) == 0) {
            nativeLayout = std::stoi(cmd.substr(15));
            return;
        }
        for (int i = 1; i <= 6; ++i) {
            const std::string prefix = "CALC1:PAR" + std::to_string(i) + ":DEF";
            if (cmd == prefix + "?") {
                reply(s, nativeParameters[static_cast<std::size_t>(i - 1)]);
                return;
            }
            if (cmd.rfind(prefix + " ", 0) == 0) {
                nativeParameters[static_cast<std::size_t>(i - 1)] = cmd.substr(prefix.size() + 1);
                return;
            }
            const std::string format = "CALC1:TRAC" + std::to_string(i) + ":FORM";
            if (cmd == format + "?") {
                reply(s, nativeFormats[static_cast<std::size_t>(i - 1)]);
                return;
            }
            if (cmd.rfind(format + " ", 0) == 0) {
                nativeFormats[static_cast<std::size_t>(i - 1)] = cmd.substr(format.size() + 1);
                return;
            }
            if (cmd == "DISP:WIND1:TRAC" + std::to_string(i) + ":Y:AUTO") return;
            const std::string marker = "CALC1:TRAC" + std::to_string(i) + ":MARK";
            if (cmd == marker + "16:STAT?") {
                reply(s, "0");
                return;
            }
            if (cmd == marker + ":COUN?") {
                reply(s, std::to_string(nativeMarkerCounts[static_cast<std::size_t>(i - 1)]));
                return;
            }
            if (cmd.rfind(marker + ":COUN ", 0) == 0) {
                nativeMarkerCounts[static_cast<std::size_t>(i - 1)] =
                    std::stoi(cmd.substr(marker.size() + 6));
                return;
            }
            if (cmd.rfind(marker, 0) == 0 && cmd.ends_with(":X?")) {
                reply(s, "1300000000");
                return;
            }
            if (cmd.rfind(marker, 0) == 0 && cmd.ends_with(":Y?")) {
                reply(s, "-12.5,0");
                return;
            }
        }
        if (cmd == "TRIG:SOUR?") {
            reply(s, triggerSourceBus ? "BUS" : "INT");
            return;
        }
        if (cmd == "INIT:CONT?") {
            reply(s, continuous ? "1" : "0");
            return;
        }
        if (cmd == "TRIG:SOUR BUS" || cmd == "TRIG:SOUR INT") {
            triggerSourceBus = (cmd == "TRIG:SOUR BUS");
            return;
        }
        if (cmd == "INIT:CONT ON" || cmd == "INIT:CONT OFF") {
            continuous = (cmd == "INIT:CONT ON");
            return;
        }

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
        if (cmd == "SENS:FREQ:STAR?") {
            reply(s, "1000000000");
            return;
        }
        if (cmd == "SENS:FREQ:STOP?") {
            reply(s, "2000000000");
            return;
        }
        if (cmd == "SENS:SWE:POIN?") {
            reply(s, "3");
            return;
        }
        if (cmd == "SENS:BAND?") {
            reply(s, "10000");
            return;
        }
        if (cmd == "SOUR:POW?") {
            reply(s, "-10");
            return;
        }
        if (cmd == "SENS:AVER?") {
            reply(s, "1");
            return;
        }
        if (cmd == "SENS:AVER:COUN?") {
            reply(s, "10");
            return;
        }
        if (cmd == "SENS:CORR:COLL:CKIT?") {
            reply(s, "1");
            return;
        }
        if (cmd == "SENS:CORR:COLL:CKIT:LAB?") {
            reply(s, "\"85032F\"");
            return;
        }
        if (cmd.rfind("SENS:CORR:COLL:SUBC ", 0) == 0) {
            calibrationSubclass = std::stoi(cmd.substr(20));
            return;
        }
        if (cmd == "SENS:CORR:COLL:SUBC?") {
            reply(s, std::to_string(calibrationSubclass.load()));
            return;
        }
        if (cmd == "SENS:CORR:STAT?") {
            reply(s, std::to_string(correction));
            return;
        }
        if (cmd == "CALC:PAR:DEF?") {
            reply(s, "S12");
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

    void wake_accept() const
    {
        if (port_ == 0) {
            return;
        }
        const StubSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kStubInvalid) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port_);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        (void)::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        stub_close(s);
    }

    void serve()
    {
        while (running_) {
            StubSocket c = ::accept(listen_, nullptr, nullptr);
            if (c == kStubInvalid) {
                break;
            }
            if (!running_) {
                stub_close(c);
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

TEST_CASE("Socket abort interrupts a blocked read", "[c2220][socket]")
{
    ScpiTcpStub stub;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    transport.set_io_timeout_ms(30000);
    transport.connect();

    std::atomic_bool interrupted{false};
    const auto started = std::chrono::steady_clock::now();
    std::thread reader([&] {
        try {
            (void)transport.read_line();
        } catch (...) {
            interrupted.store(true);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    transport.request_interrupt();
    reader.join();
    transport.abort();

    REQUIRE(interrupted.load());
    REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
}

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

    const auto& fields = vna.last_idn();
    REQUIRE(fields.manufacturer == "PLANAR");
    REQUIRE(fields.model == "C2220");
    REQUIRE(fields.serial == "STUB001");
    REQUIRE(fields.firmware == "1.0");
}

TEST_CASE("C2220Vna reuses an open socket during repeated live sweeps", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();
    vna.connect();
    const auto commands = stub.commands();
    REQUIRE(std::count(commands.begin(), commands.end(), "SYST:REC:DIR:ACC?") == 1);
    REQUIRE(vna.identify().find("C2220") != std::string::npos);
}

TEST_CASE("C2220Vna native graph restores the prior trigger", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    const auto previous = vna.enable_native_continuous();
    REQUIRE(previous.source == "BUS");
    REQUIRE_FALSE(previous.continuous);
    for (int i = 0; i < 50 && !stub.saw_exact("TRIG:SOUR INT"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(stub.saw_exact("INIT:CONT ON"));
    REQUIRE(stub.saw_exact("TRIG:SOUR INT"));
    REQUIRE(stub.continuous == 1);
    REQUIRE(stub.triggerSourceBus == 0);

    vna.restore_native_trigger(previous);
    for (int i = 0; i < 50 && !stub.saw_exact("INIT:CONT OFF"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(stub.saw_exact("TRIG:SOUR BUS"));
    REQUIRE(stub.saw_exact("INIT:CONT OFF"));
    REQUIRE(stub.continuous == 0);
    REQUIRE(stub.triggerSourceBus == 1);
}

TEST_CASE("C2220Vna restores S2VNA state and changes one native trace", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    const std::vector<std::string> oneTrace{"S11"};
    REQUIRE(vna.native_trace_parameters() == oneTrace);
    REQUIRE_THROWS(vna.set_native_trace_parameter(2, SParameter::S21));
    REQUIRE_THROWS(vna.load_state("bad\"name.sta"));

    vna.load_state("C:\\states\\21102026.sta");
    REQUIRE(stub.saw_exact("MMEM:LOAD:STAT \"C:\\states\\21102026.sta\""));
    const std::vector<std::string> fourTraces{"S11", "S21", "S12", "S22"};
    REQUIRE(vna.native_trace_parameters() == fourTraces);
    vna.set_native_trace_parameter(3, SParameter::S22);
    REQUIRE(stub.saw_exact("CALC1:PAR3:DEF S22"));
    const std::vector<std::string> changedTraces{"S11", "S21", "S22", "S22"};
    REQUIRE(vna.native_trace_parameters() == changedTraces);
}

TEST_CASE("C2220Vna controls S2VNA trace layout formats and markers", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    vna.configure_native_graph({{SParameter::S11, "MLOG"},
                                {SParameter::S21, "PHAS"},
                                {SParameter::S22, "SWR"}}, true);
    const auto state = vna.native_graph_state();
    REQUIRE(state.parameters == std::vector<std::string>{"S11", "S21", "S22"});
    REQUIRE(state.formats == std::vector<std::string>{"MLOG", "PHAS", "SWR"});
    REQUIRE(state.layout == 4);
    REQUIRE(stub.saw_exact("CALC1:PAR:COUN 3"));
    REQUIRE(stub.saw_exact("CALC1:TRAC3:FORM SWR"));
    REQUIRE(stub.saw_exact("DISP:WIND1:SPL 4"));
    REQUIRE_THROWS(vna.configure_native_graph({{SParameter::S21, "SWR"}}, false));

    vna.configure_native_graph({{SParameter::S11, "MLOG"},
                                {SParameter::S21, "MLOG"}}, false);
    REQUIRE(vna.native_graph_state().layout == 1);
    vna.restore_native_graph_state(state);
    const auto restored = vna.native_graph_state();
    REQUIRE(restored.parameters == state.parameters);
    REQUIRE(restored.formats == state.formats);
    REQUIRE(restored.layout == state.layout);
    vna.auto_scale_native_graph();
    REQUIRE(stub.saw_exact("DISP:WIND1:TRAC3:Y:AUTO"));
    vna.set_native_marker_count(1, 2);
    REQUIRE(vna.native_marker_count(1) == 2);
    const auto markers = vna.native_markers(1);
    REQUIRE(markers.size() == 2);
    REQUIRE(markers[0].frequency_hz == Approx(1.3e9));
    REQUIRE(markers[0].value == Approx(-12.5));
}

TEST_CASE("C2220Vna backs up full state and applies native sweep without changing traces", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();
    vna.save_state("C:\\states\\before.sta");
    REQUIRE(stub.saw_exact("MMEM:STOR:STYP CDST"));
    REQUIRE(stub.saw_exact("MMEM:STOR:STAT \"C:\\states\\before.sta\""));
    REQUIRE(stub.saw_exact("MMEM:STOR:STYP CST"));

    SweepConfig requested{};
    requested.f_start_hz = 4'900'000'000ULL;
    requested.f_stop_hz = 6'000'000'000ULL;
    requested.points = 5501;
    requested.ifbw_hz = 5000;
    requested.power_dbm = -40.0;
    requested.averages = 1;
    (void)vna.apply_native_sweep(requested);
    REQUIRE(stub.saw_exact("SENS:FREQ:STAR 4900000000"));
    REQUIRE(stub.saw_exact("SENS:SWE:POIN 5501"));
    REQUIRE(stub.saw_exact("SOUR:POW -40"));
    REQUIRE_FALSE(stub.saw_exact("TRIG:SOUR BUS"));
    const auto flags = vna.native_operating_state();
    REQUIRE(flags.rf_output);
    REQUIRE(flags.correction);
    const auto off = vna.set_native_operating_state(false, false);
    REQUIRE_FALSE(off.rf_output);
    REQUIRE_FALSE(off.correction);
    REQUIRE(stub.saw_exact("OUTP OFF"));
    REQUIRE(stub.saw_exact("SENS:CORR:STAT OFF"));
}

TEST_CASE("parse_scpi_idn splits manufacturer,model,serial,firmware", "[c2220]")
{
    const auto f = parse_scpi_idn(" PLANAR , C2220 , SN-42 , 26.3.1 \r\n");
    REQUIRE(f.manufacturer == "PLANAR");
    REQUIRE(f.model == "C2220");
    REQUIRE(f.serial == "SN-42");
    REQUIRE(f.firmware == "26.3.1");
    REQUIRE(f.raw == "PLANAR , C2220 , SN-42 , 26.3.1");

    const auto short_idn = parse_scpi_idn("ONLY");
    REQUIRE(short_idn.manufacturer == "ONLY");
    REQUIRE(short_idn.model.empty());
    REQUIRE(short_idn.serial.empty());
    REQUIRE(short_idn.firmware.empty());

    const auto extra = parse_scpi_idn("A,B,C,D,E");
    REQUIRE(extra.firmware == "D,E");
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
    cfg.averages = 4;
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
    REQUIRE(stub.saw_exact("SENS:AVER:COUN 4"));
    REQUIRE(stub.saw_exact("SENS:AVER ON"));
    REQUIRE(stub.saw_exact("TRIG:AVER ON"));
    REQUIRE(stub.saw_exact("TRIG:SOUR BUS"));
    REQUIRE(stub.saw_exact("SENS:AVER:CLE"));
    REQUIRE(stub.saw_exact("CALC:PAR:DEF S21"));
    REQUIRE(stub.saw_exact("CALC:PAR:SEL"));
    REQUIRE(stub.saw_exact("TRIG:SING"));
    REQUIRE(stub.saw_exact("*OPC?"));
    REQUIRE(stub.saw_exact("CALC:DATA:SDAT?"));
    REQUIRE(stub.saw_exact("CALC:DATA:XAX?"));

    const auto errs = vna.drain_errors();
    REQUIRE(errs.empty());
}

TEST_CASE("C2220Vna rejects an invalid sweep before SCPI writes", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();
    const auto commandCount = stub.commands().size();

    SweepConfig invalid{};
    invalid.f_start_hz = 100'000;
    invalid.f_stop_hz = 20'000'000'001ULL;
    invalid.points = 500'001;
    invalid.ifbw_hz = 1'000'000;
    invalid.power_dbm = 10.0;
    invalid.averages = 999;
    REQUIRE_THROWS_AS(vna.configure(invalid), std::invalid_argument);
    REQUIRE(stub.commands().size() == commandCount);

    invalid.f_stop_hz = 20'000'000'000ULL;
    REQUIRE_NOTHROW(vna.configure(invalid));
    for (int i = 0; i < 50 && !stub.saw_exact("SENS:SWE:POIN 500001"); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(stub.saw_exact("SENS:SWE:POIN 500001"));
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
    REQUIRE(stub.saw_exact("CALC:PAR:SEL"));
    REQUIRE(sweep.frequency_hz.size() == 3);
    REQUIRE(sweep.s11.size() == 3);
    REQUIRE(sweep.s21.empty());
    REQUIRE(sweep.s12.empty());
    REQUIRE(sweep.s22.empty());
    REQUIRE(sweep.s11[0].real() == Approx(1.0));
    REQUIRE_FALSE(sweep.overload);

    REQUIRE_THROWS_AS(vna.measure_s21(), std::runtime_error);
}

TEST_CASE("C2220Vna reads current S2VNA trace without reconfiguring it", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    SweepConfig instrument{};
    const auto sweep = vna.read_current_trace(&instrument);
    REQUIRE(instrument.f_start_hz == 1'000'000'000ULL);
    REQUIRE(instrument.f_stop_hz == 2'000'000'000ULL);
    REQUIRE(instrument.points == 3);
    REQUIRE(instrument.ifbw_hz == 10'000);
    REQUIRE(instrument.power_dbm == Approx(-10.0));
    REQUIRE(instrument.averages == 10);
    REQUIRE(instrument.s_parameter == SParameter::S12);
    REQUIRE(sweep.s12.size() == 3);
    REQUIRE(sweep.s11.empty());
    REQUIRE(sweep.s21.empty());
    REQUIRE(sweep.s22.empty());
    REQUIRE_FALSE(stub.saw_exact("TRIG:SING"));
    REQUIRE_FALSE(stub.saw_command_prefix("SENS:FREQ:STAR "));
    REQUIRE(stub.saw_exact("CALC:PAR:DEF?"));
}

TEST_CASE("C2220Vna reads the active S2VNA calibration kit", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    REQUIRE(vna.current_calibration_kit_id() == "CKIT 1 (85032F)");
    REQUIRE(vna.correction_enabled());
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:CKIT?"));
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:CKIT:LAB?"));
    REQUIRE(stub.saw_exact("SENS:CORR:STAT?"));
}

TEST_CASE("C2220Vna selects a documented S2VNA calibration kit", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    REQUIRE(vna.select_calibration_kit(1) == "CKIT 1 (85032F)");
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:CKIT 1"));
    REQUIRE_THROWS_AS(vna.select_calibration_kit(0), std::runtime_error);
    REQUIRE_THROWS_AS(vna.select_calibration_kit(65), std::runtime_error);
}

TEST_CASE("C2220Vna selects and verifies the calibration subclass before SOLT", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();
    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna vna(transport);
    vna.connect();

    REQUIRE_NOTHROW(vna.select_calibration_subclass(2));
    REQUIRE_NOTHROW(vna.calibrate_two_port(TwoPortCalibrationStep::OpenPort1));
    REQUIRE_NOTHROW(vna.select_calibration_subclass(1));
    REQUIRE_NOTHROW(vna.calibrate_two_port(TwoPortCalibrationStep::OpenPort2));
    REQUIRE_THROWS_AS(vna.select_calibration_subclass(0), std::runtime_error);
    REQUIRE_THROWS_AS(vna.select_calibration_subclass(9), std::runtime_error);

    const auto commands = stub.commands();
    const auto pos = [&commands](const std::string& command) {
        return std::find(commands.begin(), commands.end(), command) - commands.begin();
    };
    REQUIRE(pos("SENS:CORR:COLL:SUBC 2") < pos("SENS:CORR:COLL:OPEN 1"));
    REQUIRE(pos("SENS:CORR:COLL:SUBC 1") < pos("SENS:CORR:COLL:OPEN 2"));
    REQUIRE(pos("SENS:CORR:COLL:SUBC?") < pos("SENS:CORR:COLL:OPEN 1"));
}

TEST_CASE("C2220Vna calibrate_one_port SOLT1 sequence against stub", "[c2220]")
{
    ScpiTcpStub stub;
    stub.start();

    ScpiSocketTransport transport("127.0.0.1", stub.port());
    C2220Vna::Profile profile;
    profile.allow_direct_access = false;
    C2220Vna vna(transport, profile);
    vna.connect();
    (void)vna.identify();

    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Begin, 1));
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:METH:SOLT1 1"));
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Open, 1));
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:OPEN 1"));
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Short, 1));
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:SHOR 1"));
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Load, 1));
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:LOAD 1"));
    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Apply, 1));
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:SAVE"));

    REQUIRE_NOTHROW(vna.calibrate_one_port(OnePortCalibrationStep::Begin, 2));
    REQUIRE(stub.saw_exact("SENS:CORR:COLL:METH:SOLT1 2"));

    REQUIRE_THROWS_AS(vna.calibrate_one_port(OnePortCalibrationStep::Begin, 0),
                      std::runtime_error);
    REQUIRE_THROWS_AS(vna.calibrate_one_port(OnePortCalibrationStep::Begin, 3),
                      std::runtime_error);
}
