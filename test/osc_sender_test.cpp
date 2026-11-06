#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/osc/oscbindings.hpp"
#include "../src/core/osc/oscencoder.hpp"
#include "../src/core/osc/oscinput.hpp"
#include "../src/core/osc/osclistener.hpp"
#include "../src/core/osc/oscparser.hpp"
#include "../src/core/osc/oscsender.hpp"
#include "../src/core/osc/oscsendtable.hpp"
#include "../src/core/os/liveinputs.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "../src/core/userpublic/userinput.hpp"
#include "../src/core/userpublic/valuegraph.hpp"
#include "../src/project/graphmodule.hpp"
#include "osc_test_udp.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace Pelican;
using Catch::Approx;
using Catch::Matchers::StartsWith;
using Json = nlohmann::json;
using Kind = ValueGraph::OscSendKind;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Value = ValueGraph::OscSendValue;

template <class Predicate>
bool waitFor(Predicate predicate,
             std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return predicate();
}

void ensureLogger() {
    static const bool initialized = [] { setupLogger(); return true; }();
    (void)initialized;
}

Json sendNode(std::string id, std::string address, std::string source = "source") {
    return {{"id", std::move(id)}, {"op", "osc.send"},
            {"parameters", {{"address", std::move(address)}}},
            {"inputs", {{"value", {{"node", std::move(source)}, {"port", "value"}}}}},
            {"outputs", Json::array()}};
}

Json graphWith(Json nodes, Json outputs = Json::object()) {
    return {{"schema", "pelican.graph"}, {"version", 1},
            {"definitions", Json::object()}, {"nodes", std::move(nodes)},
            {"outputs", std::move(outputs)}};
}

Json constantNode(double value = 1.0, std::string id = "source") {
    return {{"id", std::move(id)}, {"op", "value.constant"},
            {"parameters", {{"value", value}}}, {"outputs", Json::array({"value"})}};
}

template <class Function>
std::string refusal(Function function) {
    try {
        function();
    } catch (const GraphRegistryError &error) {
        return error.code();
    }
    FAIL("graph was accepted");
    return {};
}

std::uint64_t counter(const Json &status, std::string_view name) {
    return status.at(std::string{name}).get<std::uint64_t>();
}

Value item(std::string_view address, double value, Kind kind = Kind::floating) {
    return {address, kind, value};
}

Osc::SendValue encoded(Value value) {
    const auto bits = [](Value value) {
        switch (value.kind) {
        case Kind::floating:
            return std::bit_cast<std::uint32_t>(static_cast<float>(value.value));
        case Kind::integer:
            return std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(std::round(value.value)));
        case Kind::boolean:
            return value.value == 0.0 ? 0u : 1u;
        }
        return 0u;
    }(value);
    return {value.kind, bits};
}

struct RecordedTransport {
    mutable std::mutex mutex;
    std::vector<Bytes> packets;

    OscSendTransport function() {
        return [this](std::span<const std::uint8_t> bytes) {
            std::scoped_lock lock{mutex};
            packets.emplace_back(bytes.begin(), bytes.end());
            return OscSendOutcome{};
        };
    }
    std::size_t size() const {
        std::scoped_lock lock{mutex};
        return packets.size();
    }
    std::vector<Bytes> snapshot() const {
        std::scoped_lock lock{mutex};
        return packets;
    }
    void clear() {
        std::scoped_lock lock{mutex};
        packets.clear();
    }
};

} // namespace

TEST_CASE("OSC-5b encoder round trips exact scalar messages", "[osc5b][encoder]") {
    const std::vector<std::string> addresses{"/a", "/ab", "/abc", "/abcd", "/abcdefg"};
    for (const float number : {0.1f, -0.0f, std::numeric_limits<float>::denorm_min(),
                               std::numeric_limits<float>::max()}) {
        for (const auto &address : addresses) {
            INFO(address);
            const auto bytes = Osc::encodeMessage(
                address, {Kind::floating, std::bit_cast<std::uint32_t>(number)});
            REQUIRE(bytes.size() % 4 == 0);
            const auto packet = Osc::parsePacket(bytes);
            REQUIRE(packet.drop == Osc::PacketDrop::none);
            REQUIRE(packet.messages.size() == 1);
            REQUIRE(packet.messages[0].address == address);
            REQUIRE(packet.messages[0].arguments.size() == 1);
            const auto &argument = packet.messages[0].arguments[0];
            REQUIRE(argument.tag == 'f');
            REQUIRE(std::bit_cast<std::uint32_t>(static_cast<float>(argument.number)) ==
                    std::bit_cast<std::uint32_t>(number));
        }
    }
    const auto integer = Osc::parsePacket(Osc::encodeMessage("/int", {Kind::integer, 0xfffffff9u}));
    REQUIRE(integer.drop == Osc::PacketDrop::none);
    REQUIRE(integer.messages[0].arguments[0].tag == 'i');
    REQUIRE(integer.messages[0].arguments[0].integer == -7);
    for (const bool boolean : {false, true}) {
        const auto packet = Osc::parsePacket(
            Osc::encodeMessage("/bool", {Kind::boolean, boolean ? 1u : 0u}));
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.messages[0].arguments[0].tag == (boolean ? 'T' : 'F'));
        REQUIRE(packet.messages[0].arguments[0].boolean == boolean);
        REQUIRE(Osc::encodeMessage("/bool", {Kind::boolean, boolean ? 1u : 0u}).size() == 12);
    }
}

TEST_CASE("OSC-5b lowering validates every sink including disabled ones", "[osc5b][graph]") {
    REQUIRE(builtinGraphModuleRegistry().isSink("osc.send"));
    for (const Json address : {Json{""}, Json{"a"}, Json{"/"}, Json{"/a b"},
                               Json{"/a*"}, Json{"/a?"}, Json{"/a["}, Json{"/a{"}, Json{7}}) {
        INFO(address.dump());
        auto document = graphWith(Json::array({constantNode(), sendNode("send", "/ok")}));
        document["nodes"][1]["parameters"]["address"] = address;
        REQUIRE(refusal([&] { ValueGraph::Graph graph{document.dump()}; }) == "osc.address_invalid@1");
    }
    auto document = graphWith(Json::array({constantNode(), sendNode("send", "/out")}));
    document["nodes"][1]["parameters"]["type"] = "double";
    REQUIRE(refusal([&] { ValueGraph::Graph graph{document.dump()}; }) == "osc.parameter_invalid@1");
    document["nodes"][1]["parameters"].erase("type");
    document["nodes"][1]["parameters"]["enabled"] = "yes";
    REQUIRE(refusal([&] { ValueGraph::Graph graph{document.dump()}; }) == "osc.parameter_invalid@1");
    document["nodes"][1]["parameters"].erase("enabled");
    document["nodes"][1]["parameters"]["port"] = 9000;
    REQUIRE(refusal([&] { ValueGraph::Graph graph{document.dump()}; }) == "osc.parameter_invalid@1");

    auto duplicate = graphWith(Json::array({constantNode(), sendNode("a", "/same"),
                                            sendNode("b", "/same")}));
    try {
        ValueGraph::Graph graph{duplicate.dump()};
        FAIL("duplicate addresses compiled");
    } catch (const GraphRegistryError &error) {
        REQUIRE(error.code() == "osc.address_duplicate@1");
        REQUIRE(error.detail().at("address") == "/same");
        REQUIRE(error.detail().at("nodes") == Json::array({"a", "b"}));
    }
    duplicate["nodes"][2]["parameters"]["enabled"] = false;
    const ValueGraph::Graph enabled{duplicate.dump()};
    REQUIRE(enabled.oscSendSinks().size() == 1);
    REQUIRE(enabled.oscSendSinks()[0].node == "a");

    const auto catalog = Json(builtinGraphModuleRegistry().catalog());
    bool found = false;
    for (const auto &module : catalog.at("modules")) {
        if (module.at("namespace") != "pelican.osc") continue;
        found = true;
        REQUIRE(module.at("sinks") == Json::array({"osc.send"}));
        const auto &parameters = module.at("operations")[0].at("parameters");
        REQUIRE(parameters.size() == 3);
        REQUIRE(parameters[0].at("name") == "address");
        REQUIRE(parameters[1].at("name") == "type");
        REQUIRE(parameters[1].at("enum") == Json::array({"float", "int", "bool"}));
        REQUIRE(parameters[2].at("name") == "enabled");
    }
    REQUIRE(found);
}

TEST_CASE("OSC-5b sinks publish values without binding writers", "[osc5b][graph]") {
    auto nodes = Json::array({
        Json{{"id", "clock"}, {"op", "value.input"}, {"parameters", {{"name", "time"}}},
             {"outputs", Json::array({"value"})}},
        Json{{"id", "source"}, {"op", "value.curve"},
             {"parameters", {{"keys", Json::array({Json{{"time", 0}, {"value", 0}},
                                                   Json{{"time", 1}, {"value", 1}}})}}},
             {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}},
             {"outputs", Json::array({"value"})}},
        sendNode("send", "/curve")});
    const ValueGraph::Graph graph{graphWith(nodes).dump()};
    auto state = graph.createState();
    for (const double time : {0.25, 0.75}) {
        const auto result = graph.publish(GameContext{}, {{"time", time}}, {}, {}, {}, {}, &state);
        REQUIRE(result.unavailable.empty());
        const auto values = graph.oscSendValues(result.node_values, {});
        REQUIRE(values.size() == 1);
        REQUIRE(std::string{values[0].address} == "/curve");
        REQUIRE(values[0].value == Approx(time));
    }

    const ValueGraph::Graph large{graphWith(Json::array({constantNode(1e300), sendNode("send", "/large")})).dump()};
    auto large_state = large.createState();
    const auto published = large.publish(GameContext{}, {}, {}, {}, {}, {}, &large_state);
    REQUIRE(published.unavailable.empty());
    REQUIRE(large.oscSendValues(published.node_values, {})[0].value == 1e300);

    auto unconnected = graphWith(Json::array({sendNode("send", "/zero", "missing")}));
    unconnected["nodes"][0].erase("inputs");
    const ValueGraph::Graph zero{unconnected.dump()};
    auto zero_state = zero.createState();
    const auto zero_result = zero.publish(GameContext{}, {}, {}, {}, {}, {}, &zero_state);
    REQUIRE(zero_result.unavailable.empty());
    const auto zero_values = zero.oscSendValues(zero_result.node_values, {});
    REQUIRE(zero_values.size() == 1);
    REQUIRE(zero_values[0].value == 0.0);
}

TEST_CASE("OSC-5b inactive instance sinks return no value", "[osc5b][graph]") {
    auto document = graphWith(Json::array({Json{
        {"id", "instance"}, {"op", "graph.instance"}, {"definition", "sending"},
        {"owner", {{"scene", "main"}, {"object", 1}}}, {"outputs", Json::array()}}}));
    document["definitions"]["sending"] = {
        {"nodes", Json::array({constantNode(), sendNode("send", "/instance")})},
        {"outputs", Json::object()}};
    const ValueGraph::Graph graph{document.dump()};
    REQUIRE(graph.oscSendSinks().size() == 1);
    auto state = graph.createState();
    const auto active = graph.publish(GameContext{}, {}, {}, {}, {}, {}, &state);
    REQUIRE(graph.oscSendValues(active.node_values, {}).size() == 1);
    REQUIRE(graph.oscSendValues(active.node_values, {"instance"}).empty());
    const auto inactive = graph.publish(GameContext{}, {}, {}, {}, {}, {"instance"}, &state);
    REQUIRE(graph.oscSendValues(inactive.node_values, {"instance"}).empty());
}

TEST_CASE("OSC-5b SendTable queues only encoded changes", "[osc5b][table][d1][d2][d3][d9]") {
    const Osc::Clock::time_point start{};
    Osc::SendTable table{start};
    auto result = table.applyFrame(std::array{item("/a", 1.0)});
    REQUIRE(result.changed == 1);
    REQUIRE(table.take(start).changes.size() == 1);
    REQUIRE(table.applyFrame(std::array{item("/a", 1.0)}).changed == 0);
    REQUIRE(table.hasDirty() == false);

    REQUIRE(table.applyFrame(std::array{item("/a", 0.0)}).changed == 1);
    (void)table.take(start);
    REQUIRE(table.applyFrame(std::array{item("/a", -0.0)}).changed == 1);
    (void)table.take(start);
    const double float_next = std::nextafterf(1.0f, 2.0f);
    REQUIRE(table.applyFrame(std::array{item("/a", 1.0)}).changed == 1);
    (void)table.take(start);
    REQUIRE(table.applyFrame(std::array{item("/a", float_next)}).changed == 1);
    (void)table.take(start);
    REQUIRE(table.applyFrame(std::array{item("/a", float_next)}).changed == 0);
    REQUIRE(table.applyFrame(std::array{item("/a", 1.0)}).changed == 1);
    (void)table.take(start);
    REQUIRE(table.applyFrame(std::array{item("/a", std::nextafter(1.0, 2.0))}).changed == 0);

    Osc::SendTable integers{start};
    REQUIRE(integers.applyFrame(std::array{item("/i", 1.4, Kind::integer)}).changed == 1);
    REQUIRE(integers.take(start).changes[0].value.bits == 1);
    REQUIRE(integers.applyFrame(std::array{item("/i", 1.3, Kind::integer)}).changed == 0);
    REQUIRE(integers.applyFrame(std::array{item("/i", 1.6, Kind::integer)}).changed == 1);
    REQUIRE(integers.take(start).changes[0].value.bits == 2);
    Osc::SendTable booleans{start};
    REQUIRE(booleans.applyFrame(std::array{item("/b", 0.5, Kind::boolean)}).changed == 1);
    REQUIRE(booleans.take(start).changes[0].value.bits == 1);
    REQUIRE(booleans.applyFrame(std::array{item("/b", 2.0, Kind::boolean)}).changed == 0);
    REQUIRE(booleans.applyFrame(std::array{item("/b", 0.0, Kind::boolean)}).changed == 1);
    REQUIRE(booleans.take(start).changes[0].value.bits == 0);

    Osc::SendTable kinds{start};
    REQUIRE(kinds.applyFrame(std::array{item("/same", 0.0, Kind::floating)}).changed == 1);
    (void)kinds.take(start);
    REQUIRE(kinds.applyFrame(std::array{item("/same", 0.0, Kind::integer)}).changed == 1);
}

TEST_CASE("OSC-5b rejected values stop transmission until a valid frame", "[osc5b][table][d4]") {
    const Osc::Clock::time_point start{};
    Osc::SendTable table{start};
    REQUIRE(table.applyFrame(std::array{item("/a", 2.0)}).changed == 1);
    (void)table.take(start);
    for (const double value : {std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               -std::numeric_limits<double>::infinity()}) {
        const auto result = table.applyFrame(std::array{item("/a", value)});
        REQUIRE(result.non_finite == 1);
        REQUIRE(result.changed == 0);
    }
    const auto rejected = table.take(start + std::chrono::seconds{2});
    REQUIRE(rejected.changes.empty());
    REQUIRE(rejected.resends.empty());
    const auto recovered = table.applyFrame(std::array{item("/a", 2.0)});
    REQUIRE(recovered.changed == 1);
    REQUIRE(table.take(start + std::chrono::seconds{2}).changes.size() == 1);

    Osc::SendTable ranges{start};
    const auto range = ranges.applyFrame(std::array{item("/f", 1e300), item("/i", 3e9, Kind::integer)});
    REQUIRE(range.out_of_range == 2);
    REQUIRE(range.non_finite == 0);
    REQUIRE(ranges.size() == 0);
    const auto oversized = ranges.applyFrame(std::array{item("/" + std::string(1024, 'a'), 1.0)});
    REQUIRE(oversized.dropped == 1);
    REQUIRE(ranges.size() == 0);
}

TEST_CASE("OSC-5b SendTable keeps newest values and its capacity", "[osc5b][table][d5][d6]") {
    const Osc::Clock::time_point start{};
    Osc::SendTable latest{start};
    for (const double value : {1.0, 2.0, 3.0})
        (void)latest.applyFrame(std::array{item("/a", value)});
    const auto work = latest.take(start);
    REQUIRE(work.changes.size() == 1);
    REQUIRE(work.changes[0].value == encoded(item("/a", 3.0)));

    Osc::SendTable bounded{start, 2};
    auto applied = bounded.applyFrame(std::array{item("/a", 1.0), item("/b", 2.0), item("/c", 3.0)});
    REQUIRE(applied.dropped == 1);
    REQUIRE(bounded.size() == 2);
    applied = bounded.applyFrame(std::array{item("/a", 1.0), item("/b", 2.0), item("/c", 4.0)});
    REQUIRE(applied.dropped == 1);
    applied = bounded.applyFrame(std::array{item("/a", 1.0), item("/c", 3.0)});
    REQUIRE(applied.dropped == 0);
    REQUIRE(bounded.size() == 2);
    REQUIRE(applied.changed == 1);
}

TEST_CASE("OSC-5b SendTable removes absent addresses and schedules resends", "[osc5b][table][d7][d8]") {
    const Osc::Clock::time_point start{};
    Osc::SendTable removed{start};
    (void)removed.applyFrame(std::array{item("/a", 1.0), item("/b", 2.0)});
    (void)removed.take(start);
    (void)removed.applyFrame(std::array{item("/a", 1.0)});
    const auto work = removed.take(start + Osc::send_resend_interval);
    REQUIRE(work.resends.size() == 1);
    REQUIRE(work.resends[0].address == "/a");

    Osc::SendTable table{start};
    (void)table.applyFrame(std::array{item("/a", 1.0), item("/b", 2.0)});
    (void)table.take(start);
    REQUIRE(table.take(start + std::chrono::milliseconds{999}).resends.empty());
    REQUIRE(table.take(start + std::chrono::seconds{1}).resends.size() == 2);
    REQUIRE(table.take(start + std::chrono::milliseconds{1500}).resends.empty());
    REQUIRE(table.take(start + std::chrono::seconds{2}).resends.size() == 2);
    (void)table.applyFrame(std::array{item("/a", 3.0), item("/b", 2.0)});
    const auto changed = table.take(start + std::chrono::seconds{3});
    REQUIRE(changed.changes.size() == 1);
    REQUIRE(changed.changes[0].address == "/a");
    REQUIRE(changed.resends.size() == 1);
    REQUIRE(changed.resends[0].address == "/b");
}

TEST_CASE("OSC-5b an absent sender never opens a worker or socket", "[osc5b][sender][e1]") {
    ensureLogger();
    OscSender sender;
    REQUIRE_FALSE(sender.socketOpen());
    REQUIRE_FALSE(sender.workerStarted());
    REQUIRE(sender.statusJson().at("state") == "inactive");
    REQUIRE(sender.statusJson().at("destination") == "");
    sender.publishFrame(std::array{item("/a", 1.0), item("/b", 2.0)}, false);
    REQUIRE(sender.statusJson().at("sinks") == 2);
    REQUIRE(counter(sender.statusJson(), "sent") == 0);
    sender.stop();
    sender.stop();
    REQUIRE(sender.statusJson().at("state") == "inactive");
}

TEST_CASE("OSC-5b transport errors do not stop later changes", "[osc5b][sender][e2]") {
    ensureLogger();
    OscSender sender;
    RecordedTransport recorded;
    std::atomic_size_t calls{0};
    REQUIRE(sender.startWithTransportForTesting(
        {"127.0.0.1", 9000}, [&](std::span<const std::uint8_t> bytes) {
            if (calls.fetch_add(1) == 0)
                return OscSendOutcome{OscSendOutcome::Kind::error, "injected failure"};
            std::scoped_lock lock{recorded.mutex};
            recorded.packets.emplace_back(bytes.begin(), bytes.end());
            return OscSendOutcome{};
        }));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 1.0)}, false);
        return calls.load() >= 1;
    }));
    REQUIRE(counter(sender.statusJson(), "send_errors") == 1);
    REQUIRE_THAT(sender.statusJson().at("last_error").get<std::string>(),
                 StartsWith("osc.send_failed@1"));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 2.0)}, false);
        return recorded.size() >= 1;
    }));
    REQUIRE(Osc::parsePacket(recorded.snapshot().back()).messages[0].arguments[0].number == Approx(2.0));
}

TEST_CASE("OSC-5b would-block values arrive through resend", "[osc5b][sender][e3]") {
    OscSender sender;
    RecordedTransport recorded;
    std::atomic_size_t calls{0};
    REQUIRE(sender.startWithTransportForTesting(
        {"127.0.0.1", 9000}, [&](std::span<const std::uint8_t> bytes) {
            if (calls.fetch_add(1) == 0)
                return OscSendOutcome{OscSendOutcome::Kind::would_block, "busy"};
            std::scoped_lock lock{recorded.mutex};
            recorded.packets.emplace_back(bytes.begin(), bytes.end());
            return OscSendOutcome{};
        }));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 4.0)}, false);
        return counter(sender.statusJson(), "dropped") >= 1;
    }));
    REQUIRE(waitFor([&] { return recorded.size() >= 1 && counter(sender.statusJson(), "resent") >= 1; }));
    REQUIRE(Osc::parsePacket(recorded.snapshot()[0]).messages[0].arguments[0].number == Approx(4.0));
}

TEST_CASE("OSC-5b render publication never waits for the table", "[osc5b][sender][e4]") {
    OscSender sender;
    RecordedTransport recorded;
    REQUIRE(sender.startWithTransportForTesting({"127.0.0.1", 9000}, recorded.function()));
    auto held = sender.lockTableForTesting();
    auto publication = std::async(std::launch::async, [&] {
        sender.publishFrame(std::array{item("/a", 1.0)}, false);
    });
    const bool returned = publication.wait_for(std::chrono::seconds{30}) == std::future_status::ready;
    held.unlock();
    REQUIRE(returned);
    publication.get();
    REQUIRE(counter(sender.statusJson(), "frames_busy") >= 1);
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 1.0)}, false);
        return recorded.size() == 1;
    }));
}

TEST_CASE("OSC-5b replay discards values and a fresh frame resumes", "[osc5b][sender][e5]") {
    OscSender sender;
    RecordedTransport recorded;
    REQUIRE(sender.startWithTransportForTesting({"127.0.0.1", 9000}, recorded.function()));
    for (int pass = 0; pass < 3; ++pass)
        sender.publishFrame(std::array{item("/a", 1.0)}, true);
    REQUIRE(sender.statusJson().at("state") == "replay");
    REQUIRE(sender.statusJson().at("values") == 0);
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 2.0)}, false);
        return recorded.size() >= 1;
    }));
    REQUIRE(recorded.size() == 1);
    const auto packet = Osc::parsePacket(recorded.snapshot()[0]);
    REQUIRE(packet.messages[0].arguments[0].number == Approx(2.0));
    REQUIRE(packet.messages[0].arguments[0].number != Approx(1.0));
}

TEST_CASE("OSC-5b a replay acknowledgement invalidates a taken batch", "[osc5b][sender][replay-barrier]") {
    OscSender sender;
    RecordedTransport recorded;
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool entered = false;
    bool release = false;
    REQUIRE(sender.startWithTransportForTesting(
        {"127.0.0.1", 9000}, [&](std::span<const std::uint8_t> bytes) {
            {
                std::scoped_lock lock{recorded.mutex};
                recorded.packets.emplace_back(bytes.begin(), bytes.end());
            }
            std::unique_lock lock{barrier_mutex};
            if (!entered) {
                entered = true;
                barrier_cv.notify_all();
                barrier_cv.wait(lock, [&] { return release; });
            }
            return OscSendOutcome{};
        }));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 1.0), item("/b", 2.0)}, false);
        std::scoped_lock lock{barrier_mutex};
        return entered;
    }));
    auto replay = std::async(std::launch::async, [&] { sender.pauseForReplay(); });
    REQUIRE(waitFor([&] { return sender.statusJson().at("state") == "replay"; }));
    {
        std::scoped_lock lock{barrier_mutex};
        release = true;
    }
    barrier_cv.notify_all();
    REQUIRE(replay.wait_for(std::chrono::seconds{30}) == std::future_status::ready);
    replay.get();
    REQUIRE(recorded.size() == 1);
    REQUIRE(Osc::parsePacket(recorded.snapshot()[0]).messages[0].address == "/a");
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 3.0)}, false);
        return recorded.size() >= 2;
    }));
    REQUIRE(Osc::parsePacket(recorded.snapshot()[1]).messages[0].arguments[0].number == Approx(3.0));
}

TEST_CASE("OSC-5b retirement invalidates a taken batch without another evaluation", "[osc5b][sender][retire]") {
    OscSender sender;
    RecordedTransport recorded;
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool entered = false;
    bool release = false;
    REQUIRE(sender.startWithTransportForTesting(
        {"127.0.0.1", 9000}, [&](std::span<const std::uint8_t> bytes) {
            {
                std::scoped_lock lock{recorded.mutex};
                recorded.packets.emplace_back(bytes.begin(), bytes.end());
            }
            std::unique_lock lock{barrier_mutex};
            entered = true;
            barrier_cv.notify_all();
            barrier_cv.wait(lock, [&] { return release; });
            return OscSendOutcome{};
        }));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 1.0), item("/b", 2.0)}, false);
        std::scoped_lock lock{barrier_mutex};
        return entered;
    }));
    sender.retire();
    {
        std::scoped_lock lock{barrier_mutex};
        release = true;
    }
    barrier_cv.notify_all();
    REQUIRE(waitFor([&] { return counter(sender.statusJson(), "sent") == 1; }));
    REQUIRE(sender.statusJson().at("values") == 0);
    REQUIRE(recorded.size() == 1);
    REQUIRE(recorded.snapshot()[0] == Osc::encodeMessage("/a", encoded(item("/a", 1.0))));
    const auto deadline = Osc::Clock::now() + Osc::send_resend_interval +
                          std::chrono::milliseconds{100};
    REQUIRE(waitFor([&] { return Osc::Clock::now() >= deadline; }));
    REQUIRE(recorded.size() == 1);
    REQUIRE(counter(sender.statusJson(), "resent") == 0);
}

TEST_CASE("OSC-5b rebuilding without a sink retires its table entry", "[osc5b][sender][e6]") {
    OscSender sender;
    RecordedTransport recorded;
    REQUIRE(sender.startWithTransportForTesting({"127.0.0.1", 9000}, recorded.function()));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 1.0), item("/b", 2.0)}, false);
        return sender.statusJson().at("values") == 2;
    }));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 1.0)}, false);
        return sender.statusJson().at("values") == 1;
    }));
    REQUIRE(waitFor([&] { return recorded.size() >= 2; }));
    sender.retire();
    REQUIRE(sender.statusJson().at("values") == 0);
}

TEST_CASE("OSC-5b sender stops and restarts cleanly", "[osc5b][sender][restart]") {
    OscSender sender;
    RecordedTransport recorded;
    for (int pass = 0; pass < 2; ++pass) {
        REQUIRE(sender.startWithTransportForTesting({"127.0.0.1", 9000}, recorded.function()));
        REQUIRE(waitFor([&] { return sender.workerStarted(); }));
        REQUIRE(waitFor([&] {
            sender.publishFrame(std::array{item("/restart", static_cast<double>(pass + 1))}, false);
            return recorded.size() >= static_cast<std::size_t>(pass + 1);
        }));
        sender.stop();
        REQUIRE_FALSE(sender.workerStarted());
        REQUIRE(sender.statusJson().at("state") == "inactive");
    }
    sender.stop();
    REQUIRE_FALSE(sender.socketOpen());
}

TEST_CASE("OSC-5b all deterministic launch modes suppress sending", "[osc5b][sender][e7]") {
    EngineLaunchConfig launch;
    REQUIRE_FALSE(oscSendOfflineLaunch(launch));
    const std::vector<std::pair<std::string, std::function<void(EngineLaunchConfig &)>>> modes{
        {"start_time_explicit", [](auto &value) { value.start_time_explicit = true; }},
        {"start_frames", [](auto &value) { value.start_frames = 1; }},
        {"camera_bake_output", [](auto &value) { value.camera_bake_output = "bake"; }},
        {"input_replay", [](auto &value) { value.input_replay = true; }},
        {"input_replay_path", [](auto &value) { value.input_replay_path = "input"; }},
        {"live_replay_path", [](auto &value) { value.live_replay_path = "live"; }},
        {"render_out", [](auto &value) { value.render_out = "frame"; }},
    };
    for (const auto &[name, set] : modes) {
        INFO(name);
        EngineLaunchConfig offline;
        set(offline);
        REQUIRE(oscSendOfflineLaunch(offline));
    }
    launch.output_dir = "live-outputs";
    REQUIRE_FALSE(oscSendOfflineLaunch(launch));
}

TEST_CASE("OSC-5b config is one explicit unicast IPv4 destination", "[osc5b][config]") {
    for (const auto &accepted : {std::pair{"127.0.0.1:9000", OscSendConfig{"127.0.0.1", 9000}},
                                 std::pair{"192.168.1.20:1", OscSendConfig{"192.168.1.20", 1}},
                                 std::pair{"10.0.0.1:65535", OscSendConfig{"10.0.0.1", 65535}}}) {
        INFO(accepted.first);
        REQUIRE(parseOscSendConfig(accepted.first) == accepted.second);
    }
    for (const std::string refused : {"", "9000", "localhost:9000", "127.0.0.1",
                                      "127.0.0.1:", "127.0.0.1:0", "127.0.0.1:65536",
                                      "127.0.0.1:90a", "::1:9000", "1.2.3.4:5:6",
                                      "0.0.0.0:9000", "224.0.0.1:9000",
                                      "255.255.255.255:9000"}) {
        INFO(refused);
        REQUIRE_THROWS_WITH(parseOscSendConfig(refused), StartsWith("osc.config_invalid@1: --osc-send"));
    }
}

TEST_CASE("OSC-5b real loopback UDP sends and resends all scalar kinds", "[osc5b][udp]") {
    ensureLogger();
    OscTestUdp::Receiver receiver;
    REQUIRE(receiver.port() != 0);
    OscSender sender;
    REQUIRE(sender.start({"127.0.0.1", receiver.port()}));
    REQUIRE(waitFor([&] { return sender.workerStarted(); }));
    REQUIRE(sender.statusJson().at("state") == "sending");
    REQUIRE(sender.statusJson().at("destination") ==
            "127.0.0.1:" + std::to_string(receiver.port()));
    REQUIRE(sender.socketOpen());
    const std::array values{item("/float", 0.25), item("/int", 3.0, Kind::integer),
                            item("/bool", 1.0, Kind::boolean)};
    std::map<std::string, Osc::Message> messages;
    REQUIRE(waitFor([&] {
        sender.publishFrame(values, false);
        const auto bytes = receiver.receive(std::chrono::milliseconds{50});
        if (bytes) {
            const auto packet = Osc::parsePacket(*bytes);
            REQUIRE(packet.drop == Osc::PacketDrop::none);
            REQUIRE(packet.messages.size() == 1);
            messages[packet.messages[0].address] = packet.messages[0];
        }
        return messages.size() == 3;
    }));
    REQUIRE(messages.at("/float").arguments[0].number == Approx(0.25));
    REQUIRE(messages.at("/int").arguments[0].integer == 3);
    REQUIRE(messages.at("/bool").arguments[0].boolean == true);
    const std::array changed{item("/float", 0.25), item("/int", 4.0, Kind::integer),
                             item("/bool", 1.0, Kind::boolean)};
    REQUIRE(waitFor([&] {
        sender.publishFrame(changed, false);
        const auto bytes = receiver.receive(std::chrono::milliseconds{50});
        if (bytes) {
            const auto packet = Osc::parsePacket(*bytes);
            if (packet.messages[0].address == "/int")
                messages["/int"] = packet.messages[0];
        }
        return messages.at("/int").arguments[0].integer == 4;
    }));
    bool resent_address = false;
    REQUIRE(waitFor([&] {
        const auto bytes = receiver.receive(std::chrono::milliseconds{50});
        if (bytes) {
            const auto packet = Osc::parsePacket(*bytes);
            if (packet.messages[0].address == "/float" ||
                packet.messages[0].address == "/bool")
                resent_address = true;
        }
        return resent_address && counter(sender.statusJson(), "resent") >= 1;
    }));
    REQUIRE(counter(sender.statusJson(), "sent") >= 4);
    sender.stop();
    REQUIRE_FALSE(sender.socketOpen());
}

TEST_CASE("OSC-5b an unchanged feedback value sends only on its first change", "[osc5b][feedback]") {
    ensureLogger();
    FastModuleContainer modules;
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(R"({"schema":"pelican.live_inputs","version":1,"inputs":[{"name":"phase","type":"float","default":0.0,"min":0.0,"max":1.0}]})");
    GET_MODULE(OscInput).setBindings(Osc::parseOscBindings(
        Json{{"live", Json::array({Json{{"address", "/phase"}, {"input", "phase"}}})}},
        live.declarations()));
    live.freeze(false, 1);

    OscListener listener;
    REQUIRE(listener.start({}));
    const auto port = listener.statusJson().at("port").get<std::uint16_t>();
    REQUIRE(port != 0);
    OscSender sender;
    REQUIRE(sender.start({"127.0.0.1", port}));
    OscTestUdp::Sender input;
    REQUIRE(input.valid());
    REQUIRE(input.sendTo(port, Osc::encodeMessage(
                                  "/phase", {Kind::floating,
                                             std::bit_cast<std::uint32_t>(0.1f)})));
    REQUIRE(waitFor([&] { return counter(GET_MODULE(OscInput).snapshotJson(), "applied") >= 1; }));

    std::uint64_t frame = 2;
    REQUIRE(waitFor([&] {
        live.freeze(false, frame++);
        const double phase = live.frozenInputs().at("live:phase");
        sender.publishFrame(std::array{item("/phase", phase)}, false);
        return counter(sender.statusJson(), "resent") >= 1 &&
               counter(GET_MODULE(OscInput).snapshotJson(), "received") >= 3;
    }));
    // The first packet came from the test, the second is the one changed echo,
    // and the third is its one-hertz resend. The echo itself never re-triggers.
    REQUIRE(counter(sender.statusJson(), "sent") == 1);
    listener.stop();
    sender.stop();
}

TEST_CASE("OSC-5b a busy replay frame is skipped without resuming traffic",
          "[osc5b][sender][replay-busy]") {
    OscSender sender;
    RecordedTransport recorded;
    REQUIRE(sender.startWithTransportForTesting({"127.0.0.1", 9000}, recorded.function()));
    auto held = sender.lockTableForTesting();
    auto publication = std::async(std::launch::async, [&] {
        sender.publishFrame(std::array{item("/a", 1.0)}, true);
    });
    const bool returned = publication.wait_for(std::chrono::seconds{30}) ==
                          std::future_status::ready;
    REQUIRE(counter(sender.statusJson(), "frames_busy") >= 1);
    REQUIRE(sender.statusJson().at("state") == "replay");
    held.unlock();
    REQUIRE(returned);
    publication.get();
    sender.pauseForReplay();
    REQUIRE(recorded.size() == 0);
    held = sender.lockTableForTesting();
    publication = std::async(std::launch::async, [&] {
        sender.publishFrame(std::array{item("/a", 2.0)}, false);
    });
    const bool skipped = publication.wait_for(std::chrono::seconds{30}) ==
                         std::future_status::ready;
    REQUIRE(sender.statusJson().at("state") == "replay");
    held.unlock();
    REQUIRE(skipped);
    publication.get();
    REQUIRE(recorded.size() == 0);
    sender.publishFrame(std::array{item("/a", 2.0)}, false);
    REQUIRE(waitFor([&] { return recorded.size() == 1; }));
    REQUIRE(Osc::parsePacket(recorded.snapshot()[0]).messages[0].arguments[0].number ==
            Approx(2.0));
}

TEST_CASE("OSC-5b stop discards the rest of a taken batch",
          "[osc5b][sender][stop-batch]") {
    OscSender sender;
    RecordedTransport recorded;
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    bool entered = false;
    bool release = false;
    REQUIRE(sender.startWithTransportForTesting(
        {"127.0.0.1", 9000}, [&](std::span<const std::uint8_t> bytes) {
            {
                std::scoped_lock lock{recorded.mutex};
                recorded.packets.emplace_back(bytes.begin(), bytes.end());
            }
            std::unique_lock lock{barrier_mutex};
            entered = true;
            barrier_cv.notify_all();
            barrier_cv.wait(lock, [&] { return release; });
            return OscSendOutcome{};
        }));
    REQUIRE(waitFor([&] {
        sender.publishFrame(std::array{item("/a", 1.0), item("/b", 2.0)}, false);
        std::scoped_lock lock{barrier_mutex};
        return entered;
    }));
    auto stopping = std::async(std::launch::async, [&] { sender.stop(); });
    REQUIRE(waitFor([&] { return sender.statusJson().at("stopping") == true; }));
    {
        std::scoped_lock lock{barrier_mutex};
        release = true;
    }
    barrier_cv.notify_all();
    const bool stopped = stopping.wait_for(std::chrono::seconds{30}) ==
                         std::future_status::ready;
    REQUIRE(stopped);
    stopping.get();
    REQUIRE(recorded.size() == 1);
    REQUIRE_FALSE(sender.workerStarted());
    REQUIRE_FALSE(sender.socketOpen());
}
