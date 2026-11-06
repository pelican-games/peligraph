#include "../src/core/joystick/joystickmonitor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>
#include <thread>

using namespace Pelican;
using namespace std::chrono_literals;

namespace {
const auto t0 = JoystickClock::time_point{100000ms};
using ms = JoystickClock::duration;

struct FakeJoystickSource final : JoystickSource {
    int source_calls = 0;
    int start_calls = 0;
    int present_calls = 0;
    int identify_calls = 0;
    int sample_calls = 0;
    int drain_calls = 0;
    std::optional<JoystickLimits> backend_limits;
    std::array<bool, joystick_slot_count> connected{};
    std::array<JoystickIdentity, joystick_slot_count> identities{};
    std::array<JoystickSample, joystick_slot_count> samples{};
    std::vector<JoystickConnectionChange> changes;
    std::vector<JoystickConnectionChange> during_sample;
    std::vector<JoystickConnectionChange> sample_events;
    std::optional<std::size_t> fail_sample;
    std::vector<std::thread::id> threads;

    const char *backend() const override { return "injected"; }
    std::optional<JoystickLimits> limits() const override { return backend_limits; }
    void record() {
        ++source_calls;
        threads.push_back(std::this_thread::get_id());
    }
    void startWatching() override {
        record();
        ++start_calls;
    }
    std::vector<JoystickConnectionChange> drainChanges() override {
        record();
        ++drain_calls;
        auto result = std::exchange(changes, {});
        result.insert(result.end(), during_sample.begin(), during_sample.end());
        during_sample.clear();
        return result;
    }
    bool present(std::size_t slot) override {
        record();
        ++present_calls;
        return connected.at(slot);
    }
    bool identify(std::size_t slot, JoystickIdentity &out) override {
        record();
        ++identify_calls;
        if (!connected.at(slot)) return false;
        out = identities[slot];
        return true;
    }
    bool sample(std::size_t slot, JoystickSample &out) override {
        record();
        ++sample_calls;
        if (fail_sample == slot) return false;
        out = samples[slot];
        const bool closed_with_data = !sample_events.empty();
        changes.insert(changes.end(), sample_events.begin(), sample_events.end());
        sample_events.clear();
        return connected.at(slot) || closed_with_data;
    }
    void add(std::size_t slot, std::string guid = "same", std::string name = "Pad") {
        connected[slot] = true;
        identities[slot] = {std::move(name), std::move(guid), false};
        samples[slot] = {{0.25f}, {1, 0}, {3}};
    }
    std::size_t calls() const { return static_cast<std::size_t>(source_calls); }
};

const JoystickDevice &device(const JoystickMonitor &monitor, std::size_t index = 0) {
    return monitor.snapshot()->devices.at(index);
}
} // namespace

TEST_CASE("DMAP-1a no lease and disabled monitors cost nothing", "[dmap1a]") {
    FakeJoystickSource source;
    JoystickMonitor monitor{&source};
    for (int i = 0; i < 100; ++i) monitor.pumpFrame(t0 + ms{i});
    CHECK(source.source_calls == 0);
    CHECK(source.drain_calls == 0);
    CHECK(monitor.publishCount() == 0);
    CHECK(monitor.snapshot()->devices.empty());

    JoystickMonitor disabled{nullptr};
    CHECK_FALSE(disabled.enabled());
    CHECK(std::string{disabled.backend()} == "none");
    disabled.renewLease(t0);
    disabled.pumpFrame(t0);
    const auto reply = joystickDevicesJson(disabled, t0, true, "engine");
    CHECK_FALSE(reply.at("enabled").get<bool>());
    CHECK(reply.at("devices").empty());
}

TEST_CASE("DMAP-1a first lease starts once and samples each device", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(2);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    for (int i = 0; i < 4; ++i) monitor.pumpFrame(t0 + ms{i});
    CHECK(source.start_calls == 1);
    CHECK(source.present_calls == 16);
    CHECK(source.sample_calls == 4);
    CHECK(monitor.snapshot()->sample_serial == 4);
    CHECK(device(monitor).instance_id == 1);
    CHECK(device(monitor).generation == 1);
}

TEST_CASE("DMAP-1a reconnects never reuse identity", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(3, "old");
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    const auto first = device(monitor).instance_id;
    source.changes.push_back({3, false});
    monitor.pumpFrame(t0 + 1ms);
    CHECK(monitor.snapshot()->devices.empty());
    CHECK(monitor.snapshot()->generation > 1);
    source.changes.push_back({3, true});
    monitor.pumpFrame(t0 + 2ms);
    CHECK(device(monitor).instance_id != first);
    CHECK(device(monitor).generation > 2);

    const auto second = device(monitor).instance_id;
    source.identities[3].guid = "new";
    source.changes = {{3, false}, {3, true}};
    monitor.pumpFrame(t0 + 3ms);
    CHECK(device(monitor).instance_id != second);
    CHECK(device(monitor).identity.guid == "new");
    CHECK(device(monitor).values.axes[0] == 0.25f);

    const auto third = device(monitor).instance_id;
    source.identities[3].guid = "replacement";
    source.changes = {{3, true}};
    monitor.pumpFrame(t0 + 4ms);
    CHECK(device(monitor).instance_id != third);
    CHECK(device(monitor).identity.guid == "replacement");
    const auto generation = monitor.snapshot()->generation;
    source.changes = {{9, false}};
    monitor.pumpFrame(t0 + 5ms);
    CHECK(monitor.snapshot()->generation == generation);
}

TEST_CASE("DMAP-1a identical GUIDs remain separate instances", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(1);
    source.add(4);
    source.samples[4].axes[0] = 0.75f;
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    REQUIRE(monitor.snapshot()->devices.size() == 2);
    CHECK(device(monitor, 0).instance_id != device(monitor, 1).instance_id);
    CHECK(device(monitor, 0).values.axes[0] == 0.25f);
    CHECK(device(monitor, 1).values.axes[0] == 0.75f);
}

TEST_CASE("DMAP-1a lease expiry freezes values but not hotplug identity", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    const auto instance = device(monitor).instance_id;
    monitor.pumpFrame(t0 + 1999ms);
    CHECK(monitor.leaseActive(t0 + 1999ms));
    const auto samples = source.sample_calls;
    monitor.pumpFrame(t0 + 2000ms);
    CHECK_FALSE(monitor.leaseActive(t0 + 2000ms));
    CHECK(source.sample_calls == samples);
    CHECK(device(monitor).instance_id == instance);
    CHECK(device(monitor).sampled);
    CHECK(device(monitor).axes == 1);
    CHECK_FALSE(device(monitor).has_values);
    CHECK(device(monitor).values.axes.empty());
    const auto published = monitor.publishCount();
    for (int i = 0; i < 100; ++i) monitor.pumpFrame(t0 + 2001ms + ms{i});
    CHECK(source.sample_calls == samples);
    CHECK(source.present_calls == 16);
    CHECK(monitor.publishCount() == published);

    source.add(5);
    source.changes = {{5, true}};
    monitor.pumpFrame(t0 + 3000ms);
    CHECK(monitor.snapshot()->devices.size() == 2);
    CHECK(source.identify_calls == 2);
    CHECK(source.sample_calls == samples);
    CHECK_FALSE(device(monitor, 1).sampled);
    monitor.renewLease(t0 + 4000ms);
    monitor.pumpFrame(t0 + 4000ms);
    CHECK(source.sample_calls > samples);
    CHECK(device(monitor).instance_id == instance);
}

TEST_CASE("DMAP-1a renewal is monotonic and stale values need a fresh pump", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0 + 1000ms);
    monitor.renewLease(t0);
    CHECK(monitor.leaseActive(t0 + 2999ms));
    CHECK_FALSE(monitor.leaseActive(t0 + 3000ms));
    monitor.pumpFrame(t0);
    const auto old_values = device(monitor).values.axes;
    REQUIRE_FALSE(old_values.empty());
    monitor.pumpFrame(t0 + 3000ms);
    CHECK(device(monitor).values.axes.empty());
    monitor.renewLease(t0 + 5000ms);
    auto reply = joystickDevicesJson(monitor, t0 + 5000ms, true, "engine");
    CHECK(reply.at("monitoring").get<bool>());
    CHECK_FALSE(reply.at("devices")[0].contains("values"));
    monitor.pumpFrame(t0 + 5000ms);
    reply = joystickDevicesJson(monitor, t0 + 5000ms, true, "engine");
    CHECK(reply.at("devices")[0].contains("values"));
}

TEST_CASE("DMAP-1a failed samples preserve identity and recover", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    const auto instance = device(monitor).instance_id;
    const auto generation = device(monitor).generation;
    source.fail_sample = 0;
    monitor.pumpFrame(t0 + 1ms);
    CHECK(device(monitor).instance_id == instance);
    CHECK(device(monitor).generation == generation);
    CHECK(device(monitor).sampled);
    CHECK(device(monitor).axes == 1);
    CHECK_FALSE(device(monitor).has_values);
    CHECK(device(monitor).values.axes.empty());
    source.fail_sample.reset();
    monitor.pumpFrame(t0 + 2ms);
    CHECK(device(monitor).instance_id == instance);
    CHECK(device(monitor).has_values);
    CHECK(device(monitor).values.axes[0] == 0.25f);
}

TEST_CASE("DMAP-1a sampled disconnect callbacks remain authoritative", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    source.connected[0] = false;
    source.sample_events = {{0, false}};
    monitor.pumpFrame(t0 + 1ms);
    CHECK(monitor.snapshot()->devices.size() == 1);
    monitor.pumpFrame(t0 + 2ms);
    CHECK(monitor.snapshot()->devices.empty());
    CHECK(monitor.snapshot()->generation == 2);
    source.during_sample = {{0, true}, {0, false}, {0, true}};
    source.connected[0] = true;
    monitor.pumpFrame(t0 + 2ms);
    REQUIRE(monitor.snapshot()->devices.size() == 1);
    CHECK(device(monitor).instance_id == 3);
    CHECK(monitor.snapshot()->generation == 5);
}

TEST_CASE("DMAP-1a snapshots own their values", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    const auto first = monitor.snapshot();
    source.samples[0].axes[0] = 0.9f;
    monitor.pumpFrame(t0 + 1ms);
    const auto second = monitor.snapshot();
    CHECK(first != second);
    CHECK(first->devices[0].values.axes[0] == 0.25f);
    CHECK(second->devices[0].values.axes[0] == 0.9f);
}

TEST_CASE("DMAP-1a source calls have owner thread affinity", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    const auto calls = source.calls();
    std::thread reader{[&] {
        monitor.renewLease(t0 + 1ms);
        CHECK(monitor.leaseActive(t0 + 1ms));
        CHECK(monitor.snapshot()->devices.size() == 1);
        CHECK(joystickDevicesJson(monitor, t0 + 1ms, false, "engine").at("source_kind") == "joy");
        CHECK_THROWS_AS(monitor.pumpFrame(t0 + 1ms), std::logic_error);
    }};
    reader.join();
    CHECK(source.calls() == calls);
    CHECK(std::ranges::all_of(source.threads, [&](const auto id) { return id == std::this_thread::get_id(); }));
}

TEST_CASE("DMAP-1a concurrent readers see coherent immutable snapshots", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    std::atomic<bool> done = false;
    std::atomic<bool> valid = true;
    std::uint64_t last_generation = 0;
    std::thread reader{[&] {
        while (!done.load()) {
            monitor.renewLease(t0 + 1000ms);
            const auto current = monitor.snapshot();
            for (const auto &entry : current->devices) {
                if (entry.has_values && entry.values.axes.size() != entry.axes) valid = false;
            }
            if (current->generation < last_generation) valid = false;
            last_generation = current->generation;
        }
    }};
    for (int i = 0; i < 2000; ++i) {
        if (i % 50 == 0) {
            source.connected[0] = true;
            source.changes.push_back({0, true});
        } else if (i % 50 == 25) {
            source.connected[0] = false;
            source.changes.push_back({0, false});
        }
        monitor.pumpFrame(t0 + ms{i});
    }
    done = true;
    reader.join();
    CHECK(valid.load());
    CHECK(monitor.snapshot()->sample_serial == 2000);
}

TEST_CASE("DMAP-1a invalid floats and UTF-8 are repaired", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0, "guid", std::string{"Pad\xE3\x81"});
    source.samples[0].axes = {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(), 2.0f};
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    const auto reply = joystickDevicesJson(monitor, t0, true, "engine");
    CHECK_NOTHROW(reply.dump());
    CHECK(reply.at("devices")[0].at("name").get<std::string>().find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(reply.at("devices")[0].at("values").at("axes")[0] == 0.0f);
    CHECK(reply.at("devices")[0].at("values").at("axes")[1] == 0.0f);
    CHECK(reply.at("devices")[0].at("values").at("axes")[2] == 2.0f);
}

TEST_CASE("DMAP-1a real buttons exclude synthetic hat buttons", "[dmap1a]") {
    CHECK(joystickRealButtonCount(16, 1) == 12);
    CHECK(joystickRealButtonCount(10, 0) == 10);
    CHECK(joystickRealButtonCount(2, 1) == 0);
    CHECK(joystickRealButtonCount(-1, 1) == 0);
}

TEST_CASE("DMAP-1a backend limits warn about possible truncation", "[dmap1a]") {
    FakeJoystickSource source;
    source.backend_limits = JoystickLimits{8, 32, 4};
    source.add(0);
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    auto check = [&](std::size_t axes, std::size_t buttons, std::size_t hats, bool expected) {
        source.samples[0] = {std::vector<float>(axes), std::vector<std::uint8_t>(buttons),
                             std::vector<std::uint8_t>(hats)};
        monitor.pumpFrame(t0 + ms{1 + axes + buttons + hats});
        CHECK(device(monitor).maybe_truncated == expected);
    };
    check(0, 31, 0, false);
    check(0, 32, 0, true);
    check(0, 0, 3, false);
    check(0, 0, 4, true);
    check(7, 0, 0, false);
    check(8, 0, 0, true);
    source.backend_limits.reset();
    check(0, 64, 0, false);
}

TEST_CASE("DMAP-1a reply shape separates peeks from current values", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    JoystickMonitor monitor{&source};
    auto peek = joystickDevicesJson(monitor, t0, false, "engine");
    for (const char *key : {"enabled", "backend", "source_kind", "engine_instance_id",
                            "monitoring", "lease_ms", "generation", "sample_serial", "devices"}) {
        CHECK(peek.contains(key));
    }
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    auto values = joystickDevicesJson(monitor, t0, true, "engine");
    REQUIRE(values.at("devices").size() == 1);
    CHECK(values.at("source_kind") == "joy");
    CHECK(values.at("engine_instance_id") == "engine");
    CHECK_FALSE(values.at("devices")[0].contains("slot"));
    CHECK(values.at("devices")[0].contains("values"));
    monitor.pumpFrame(t0 + 2000ms);
    CHECK_FALSE(joystickDevicesJson(monitor, t0 + 2000ms, true, "engine")
                    .at("devices")[0].contains("values"));
    peek = joystickDevicesJson(monitor, t0 + 2000ms, false, "engine");
    CHECK_FALSE(peek.at("devices")[0].contains("values"));
    CHECK_FALSE(monitor.leaseActive(t0 + 2000ms));

    FakeJoystickSource unsampled;
    unsampled.add(0);
    unsampled.fail_sample = 0;
    JoystickMonitor before{&unsampled};
    before.renewLease(t0);
    before.pumpFrame(t0);
    CHECK_FALSE(joystickDevicesJson(before, t0, true, "engine")
                    .at("devices")[0].contains("values"));
}

TEST_CASE("DMAP-1a injected source validates and scans without duplicates", "[dmap1a]") {
    InjectedJoystickSource source;
    source.connect(0, {"Pad", "guid", false}, {{1.0f}, {1}, {0}});
    JoystickMonitor monitor{&source};
    monitor.renewLease(t0);
    monitor.pumpFrame(t0);
    REQUIRE(monitor.snapshot()->devices.size() == 1);
    CHECK(monitor.snapshot()->generation == 1);
    CHECK(device(monitor).values.axes[0] == 1.0f);
    CHECK_THROWS_AS(source.connect(0, {}, {}), std::invalid_argument);
    CHECK_THROWS_AS(source.set(1, {}), std::invalid_argument);
    CHECK_THROWS_AS(source.disconnect(1), std::invalid_argument);
    CHECK_THROWS_AS(source.connect(16, {}, {}), std::invalid_argument);
}

TEST_CASE("DMAP-1a events wait for a frame and identify failure is transient", "[dmap1a]") {
    FakeJoystickSource source;
    source.add(0);
    source.changes = {{0, true}};
    JoystickMonitor monitor{&source};
    monitor.pumpFrame(t0);
    CHECK(source.source_calls == 0);
    CHECK(monitor.snapshot()->devices.empty());
    source.connected[0] = false;
    monitor.renewLease(t0);
    monitor.pumpFrame(t0 + 1ms);
    CHECK(monitor.snapshot()->devices.empty());
    source.connected[0] = true;
    source.changes = {{0, true}};
    monitor.pumpFrame(t0 + 2ms);
    CHECK(monitor.snapshot()->devices.size() == 1);
}
