#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/osc/oscbindings.hpp"
#include "../src/core/osc/oscinput.hpp"
#include "../src/core/osc/osclistener.hpp"
#include "../src/core/osc/osclistenerconfig.hpp"
#include "../src/core/os/liveinputs.hpp"
#include "../src/core/userpublic/userinput.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace Pelican;
using Catch::Matchers::StartsWith;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::string_view liveDeclaration = R"({
  "schema": "pelican.live_inputs", "version": 1,
  "inputs": [
    {"name": "phase", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0}
  ]
})";

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

void putU32(Bytes &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putU64(Bytes &out, std::uint64_t value) {
    putU32(out, static_cast<std::uint32_t>(value >> 32));
    putU32(out, static_cast<std::uint32_t>(value));
}

Bytes bundleFor(std::uint64_t timetag, const Bytes &message) {
    Bytes out{'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
    putU64(out, timetag);
    putU32(out, static_cast<std::uint32_t>(message.size()));
    out.insert(out.end(), message.begin(), message.end());
    return out;
}

Bytes messageFor(std::string_view address, float value) {
    Bytes out(address.begin(), address.end());
    out.push_back(0);
    while ((out.size() % 4) != 0) out.push_back(0);
    out.insert(out.end(), {',', 'f', 0, 0});
    putU32(out, std::bit_cast<std::uint32_t>(value));
    return out;
}

std::uint64_t counter(const nlohmann::json &snapshot, std::string_view key) {
    return snapshot.at(std::string{key}).get<std::uint64_t>();
}

template <class Predicate>
bool waitFor(Predicate predicate,
             std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return predicate();
}

void configureLive() {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(liveDeclaration);
    GET_MODULE(OscInput).setBindings(Osc::parseOscBindings(
        nlohmann::json{{"live", nlohmann::json::array({nlohmann::json{
                                  {"address", "/phase"}, {"input", "phase"}}})}},
        live.declarations()));
    live.freeze(false, 1);
}

#if defined(_WIN32)
using TestSocket = SOCKET;
constexpr TestSocket invalid_test_socket = INVALID_SOCKET;
#else
using TestSocket = int;
constexpr TestSocket invalid_test_socket = -1;
#endif

#if defined(_WIN32)
struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        ok = ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockScope() {
        if (ok) ::WSACleanup();
    }
    bool ok = false;
};

WinsockScope &testWinsock() {
    static WinsockScope scope;
    return scope;
}
#endif

class TestUdpSender {
  public:
    TestUdpSender() {
#if defined(_WIN32)
        if (!testWinsock().ok) return;
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#endif
        if (socket_ == invalid_test_socket) return;

        int send_buffer = 1 << 20;
        (void)::setsockopt(socket_, SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<const char *>(&send_buffer),
                           sizeof(send_buffer));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(0);
        local.sin_addr.s_addr = htonl(0x7f000001u);
        if (::bind(socket_, reinterpret_cast<const sockaddr *>(&local),
                   sizeof(local)) != 0) {
            close();
        }
    }

    ~TestUdpSender() { close(); }

    TestUdpSender(const TestUdpSender &) = delete;
    TestUdpSender &operator=(const TestUdpSender &) = delete;

    [[nodiscard]] bool valid() const { return socket_ != invalid_test_socket; }

    [[nodiscard]] std::uint16_t localPort() const {
        sockaddr_in local{};
#if defined(_WIN32)
        int length = sizeof(local);
#else
        socklen_t length = sizeof(local);
#endif
        if (::getsockname(socket_, reinterpret_cast<sockaddr *>(&local),
                          &length) != 0)
            return 0;
        return ntohs(local.sin_port);
    }

    bool sendTo(std::uint16_t port, std::span<const std::uint8_t> bytes) const {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(port);
        destination.sin_addr.s_addr = htonl(0x7f000001u);
#if defined(_WIN32)
        const int sent =
            ::sendto(socket_, reinterpret_cast<const char *>(bytes.data()),
                     static_cast<int>(bytes.size()), 0,
                     reinterpret_cast<const sockaddr *>(&destination),
                     sizeof(destination));
#else
        const auto sent =
            ::sendto(socket_, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<const sockaddr *>(&destination),
                     sizeof(destination));
#endif
        return sent >= 0 &&
               static_cast<std::size_t>(sent) == bytes.size();
    }

  private:
    void close() {
        if (socket_ == invalid_test_socket) return;
#if defined(_WIN32)
        ::closesocket(socket_);
#else
        ::close(socket_);
#endif
        socket_ = invalid_test_socket;
    }

    TestSocket socket_ = invalid_test_socket;
};

std::string optionError(
    std::string_view port, std::string_view interface_address,
    std::optional<std::string_view> allow = std::nullopt) {
    try {
        (void)parseOscListenerConfig(port, interface_address, allow);
        return {};
    } catch (const std::runtime_error &error) {
        return std::string{error.what()};
    }
}

} // namespace

TEST_CASE("OSC-2b options validate the port, interface and allow list",
          "[osc2b][osc][config]") {
    const auto defaults = parseOscListenerConfig("0", "127.0.0.1", std::nullopt);
    REQUIRE(defaults.port == 0);
    REQUIRE(defaults.interface_address == "127.0.0.1");
    REQUIRE(defaults.allow.empty());

    const auto configured = parseOscListenerConfig(
        "9000", "0.0.0.0",
        std::optional<std::string_view>{"192.168.1.20,127.0.0.2"});
    REQUIRE(configured.port == 9000);
    REQUIRE(configured.interface_address == "0.0.0.0");
    REQUIRE(configured.allow ==
            std::vector<std::string>{"192.168.1.20", "127.0.0.2"});

    REQUIRE(parseOscListenerConfig("65535", "127.0.0.1", std::nullopt).port ==
            65535);

    for (const auto port : {"", "-1", "+1", "65536", "1.5", "abc", " 1", "1 "}) {
        const auto message = optionError(port, "127.0.0.1");
        INFO(port);
        REQUIRE_THAT(message, StartsWith("osc.config_invalid@1:"));
    }
    for (const auto interface_address :
         {"", "localhost", "::1", "127.0.0", "127.0.0.1.2", "999.1.1.1",
          " 127.0.0.1", "127.0.0.1 "}) {
        const auto message = optionError("9000", interface_address);
        INFO(interface_address);
        REQUIRE_THAT(message, StartsWith("osc.config_invalid@1:"));
    }
    for (const auto allow :
         {"", "127.0.0.1,", ",127.0.0.1", "localhost", "127.0.0.1,,127.0.0.2",
          "999.1.1.1", "1.2.3", "::1"}) {
        const auto message =
            optionError("9000", "127.0.0.1", std::optional<std::string_view>{allow});
        INFO(allow);
        REQUIRE_THAT(message, StartsWith("osc.config_invalid@1:"));
    }
}

TEST_CASE("OSC-2b without --osc-port opens no socket and starts no worker",
          "[osc2b][osc][listener][off]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(EngineLaunchConfig).osc_listener.reset();

    internal::prepareOscListener();
    auto &listener = GET_MODULE(OscListener);
    REQUIRE(listener.statusJson().at("state").get<std::string>() == "off");
    REQUIRE_FALSE(listener.socketOpen());
    REQUIRE_FALSE(listener.workerStarted());

    // The composition root may be reached more than once; that must not turn
    // an unconfigured listener into a bound one.
    internal::prepareOscListener();
    REQUIRE(listener.statusJson().at("state").get<std::string>() == "off");
    REQUIRE_FALSE(listener.socketOpen());
}

TEST_CASE("OSC-2b loopback port 0 receives a datagram and freezes its value",
          "[osc2b][osc][listener]") {
    ensureLogger();
    FastModuleContainer modules;
    configureLive();

    OscListener listener;
    OscListenerConfig config;
    config.port = 0;
    REQUIRE(listener.start(config));
    REQUIRE(listener.statusJson().at("state").get<std::string>() ==
            "listening");
    REQUIRE(listener.socketOpen());
    REQUIRE(listener.workerStarted());
    const auto bound_port =
        listener.statusJson().at("port").get<std::uint16_t>();
    REQUIRE(bound_port != 0);

    std::string endpoint_address;
    std::uint16_t endpoint_port = 0;
    REQUIRE(listener.localEndpoint(endpoint_address, endpoint_port));
    REQUIRE(endpoint_address == "127.0.0.1");
    REQUIRE(endpoint_port == bound_port);

    TestUdpSender sender;
    REQUIRE(sender.valid());
    REQUIRE(sender.sendTo(bound_port, messageFor("/phase", 0.75f)));

    auto &osc = GET_MODULE(OscInput);
    REQUIRE(waitFor([&] {
        const auto snapshot = osc.snapshotJson();
        return counter(snapshot, "received") >= 1 &&
               counter(snapshot, "applied") >= 1;
    }));

    auto &live = GET_MODULE(LiveInputs);
    live.freeze(false, 2);
    REQUIRE(live.frozenInputs().at("live:phase") == Catch::Approx(0.75));

    const auto snapshot = osc.snapshotJson();
    REQUIRE(counter(snapshot, "received") == 1);
    REQUIRE(snapshot.at("recent").back().at("sender").get<std::string>() ==
            "127.0.0.1:" + std::to_string(sender.localPort()));
}

TEST_CASE("OSC-2b the default interface binds loopback, not every interface",
          "[osc2b][osc][listener][bind]") {
    ensureLogger();
    FastModuleContainer modules;

    OscListener listener;
    OscListenerConfig config;
    config.port = 0; // interface_address keeps its 127.0.0.1 default.
    REQUIRE(listener.start(config));

    std::string address;
    std::uint16_t port = 0;
    REQUIRE(listener.localEndpoint(address, port));
    REQUIRE(address == "127.0.0.1");
    REQUIRE(port != 0);
    REQUIRE(listener.statusJson().at("interface").get<std::string>() ==
            "127.0.0.1");
}

TEST_CASE("OSC-2b a busy port fails without reuse and the first listener lives",
          "[osc2b][osc][listener][bind]") {
    ensureLogger();
    FastModuleContainer modules;
    configureLive();

    OscListener first;
    OscListenerConfig config;
    config.port = 0;
    REQUIRE(first.start(config));
    const auto port = first.statusJson().at("port").get<std::uint16_t>();

#if !defined(_WIN32)
    REQUIRE_FALSE(first.reuseAddressOptionForTesting());
#endif

    OscListener second;
    config.port = port;
    REQUIRE_FALSE(second.start(config));
    const auto failed = second.statusJson();
    REQUIRE(failed.at("state").get<std::string>() == "failed");
    REQUIRE_THAT(failed.at("error").get<std::string>(),
                 StartsWith("osc.bind_failed@1:"));
    REQUIRE_FALSE(second.socketOpen());
    REQUIRE_FALSE(second.workerStarted());

    TestUdpSender sender;
    REQUIRE(sender.valid());
    REQUIRE(sender.sendTo(port, messageFor("/phase", 0.25f)));
    auto &osc = GET_MODULE(OscInput);
    REQUIRE(waitFor(
        [&] { return counter(osc.snapshotJson(), "received") >= 1; }));
    auto &live = GET_MODULE(LiveInputs);
    live.freeze(false, 2);
    REQUIRE(live.frozenInputs().at("live:phase") == Catch::Approx(0.25));
}

TEST_CASE("OSC-2b allow is an exact sender IPv4 filter, not authentication",
          "[osc2b][osc][listener][allow]") {
    ensureLogger();
    FastModuleContainer modules;
    configureLive();

    OscListener rejected;
    OscListenerConfig reject_config;
    reject_config.port = 0;
    reject_config.allow = {"127.0.0.2"};
    REQUIRE(rejected.start(reject_config));

    TestUdpSender sender;
    REQUIRE(sender.valid());
    REQUIRE(sender.sendTo(
        rejected.statusJson().at("port").get<std::uint16_t>(),
        messageFor("/phase", 0.9f)));
    REQUIRE(waitFor(
        [&] {
            return counter(rejected.statusJson(), "sender_not_allowed") >= 1;
        },
        std::chrono::seconds{5}));

    auto &live = GET_MODULE(LiveInputs);
    live.freeze(false, 2);
    REQUIRE(live.frozenInputs().at("live:phase") == Catch::Approx(0.0));

    OscListener accepted;
    OscListenerConfig accept_config;
    accept_config.port = 0;
    accept_config.allow = {"127.0.0.1"};
    REQUIRE(accepted.start(accept_config));
    REQUIRE(sender.sendTo(
        accepted.statusJson().at("port").get<std::uint16_t>(),
        messageFor("/phase", 0.8f)));

    auto &osc = GET_MODULE(OscInput);
    REQUIRE(waitFor([&] {
        const auto snapshot = osc.snapshotJson();
        return counter(snapshot, "received") >= 1 &&
               counter(snapshot, "applied") >= 1;
    }));
    live.freeze(false, 3);
    REQUIRE(live.frozenInputs().at("live:phase") == Catch::Approx(0.8));
}

TEST_CASE("OSC-2b stop returns promptly, is idempotent and destruction joins",
          "[osc2b][osc][listener][stop]") {
    ensureLogger();
    FastModuleContainer modules;

    auto listener = std::make_unique<OscListener>();
    OscListenerConfig config;
    config.port = 0;
    REQUIRE(listener->start(config));

    std::atomic_bool stopped{false};
    std::thread stopper([&] {
        listener->stop();
        stopped.store(true, std::memory_order_release);
    });
    const bool returned = waitFor(
        [&] { return stopped.load(std::memory_order_acquire); },
        std::chrono::seconds{10});
    if (!returned) {
        // A blocking receive mutation leaves the join stuck forever.  Detach
        // it and leak the listener so the failure is reported, not hidden by
        // the test process hanging.
        stopper.detach();
        (void)listener.release();
        FAIL("stop() did not return within 10 seconds");
    }
    stopper.join();
    REQUIRE_FALSE(listener->workerStarted());
    REQUIRE_FALSE(listener->socketOpen());
    listener->stop();

    auto second = std::make_unique<OscListener>();
    REQUIRE(second->start(config));
    const auto port = second->statusJson().at("port").get<std::uint16_t>();

    std::atomic_bool sending{true};
    std::thread sender_thread([&] {
        TestUdpSender sender;
        while (sending.load(std::memory_order_acquire)) {
            (void)sender.sendTo(port, messageFor("/phase", 0.5f));
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    second.reset();
    sending.store(false, std::memory_order_release);
    sender_thread.join();
}

TEST_CASE("OSC-2b a maximum UDP payload is accepted and counted as malformed",
          "[osc2b][osc][listener][size]") {
    ensureLogger();
    FastModuleContainer modules;

    OscListener listener;
    OscListenerConfig config;
    config.port = 0;
    REQUIRE(listener.start(config));
    const auto port = listener.statusJson().at("port").get<std::uint16_t>();

    TestUdpSender sender;
    REQUIRE(sender.valid());
    const Bytes maximum_payload(65507, 0);
    REQUIRE(sender.sendTo(port, maximum_payload));

    auto &osc = GET_MODULE(OscInput);
    REQUIRE(waitFor([&] {
        const auto snapshot = osc.snapshotJson();
        return counter(snapshot, "received") >= 1 &&
               counter(snapshot.at("dropped"), "malformed") >= 1;
    }));
}


TEST_CASE("OSC-5a the listener releases a real UDP bundle on its timeout path",
          "[osc5a][osc][listener]") {
    ensureLogger();
    FastModuleContainer modules;
    configureLive();
    OscListener listener;
    OscListenerConfig config;
    config.port = 0;
    REQUIRE(listener.start(config));
    const auto port = listener.statusJson().at("port").get<std::uint16_t>();
    TestUdpSender sender;
    REQUIRE(sender.valid());

    constexpr std::uint64_t ahead_ticks =
        (50ULL * (1ULL << 32) + 999ULL) / 1000ULL;
    const auto sent_at = std::chrono::steady_clock::now();
    REQUIRE(sender.sendTo(port, bundleFor(ntpNowUtc() + ahead_ticks,
                                          messageFor("/phase", 0.625f))));
    auto &osc = GET_MODULE(OscInput);
    REQUIRE(waitFor([&] { return osc.snapshotJson().at("scheduled") == 1; },
                    std::chrono::seconds{2}));
    REQUIRE(osc.snapshotJson().at("applied") == 0);
    auto &live = GET_MODULE(LiveInputs);
    live.freeze(false, 2);
    REQUIRE(live.frozenInputs().at("live:phase") == Catch::Approx(0.0));
    REQUIRE(waitFor([&] { return osc.snapshotJson().at("applied") == 1; },
                    std::chrono::seconds{5}));
    const auto delay = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - sent_at).count() - 50.0;
    std::cout << "OSC-5a listener release delay: " << delay
              << " ms (before the next render frame)\n";
    REQUIRE(osc.snapshotJson().at("recent").back().at("scheduled") == true);
    live.freeze(false, 3);
    REQUIRE(live.frozenInputs().at("live:phase") == Catch::Approx(0.625));
}

TEST_CASE("OSC-5a stopping the listener discards its held UDP bundle",
          "[osc5a][osc][listener][stop]") {
    ensureLogger();
    FastModuleContainer modules;
    configureLive();
    OscListener listener;
    OscListenerConfig config;
    config.port = 0;
    REQUIRE(listener.start(config));
    const auto port = listener.statusJson().at("port").get<std::uint16_t>();
    TestUdpSender sender;
    REQUIRE(sender.valid());
    REQUIRE(sender.sendTo(port, bundleFor(ntpNowUtc() + (5ULL << 32),
                                          messageFor("/phase", 0.875f))));
    auto &osc = GET_MODULE(OscInput);
    REQUIRE(waitFor([&] { return osc.snapshotJson().at("scheduled") == 1; },
                    std::chrono::seconds{2}));
    listener.stop();
    REQUIRE(osc.snapshotJson().at("scheduled") == 0);
    REQUIRE(osc.snapshotJson().at("applied") == 0);
}
