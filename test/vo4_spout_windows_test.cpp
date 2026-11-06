// VO-4: real spoutDX in one process, a sender on its own thread and a receiver
// on the test's. A Windows D3D11 integration test, not a GPU-free one: it needs
// a working device in an interactive session, and without one it skips, which
// isn't a pass. No HWND, COM or message loop is involved.
#include "../src/core/output/outputspout.hpp"
#include "spout_test_receiver.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Pelican::OutputSpoutSender;

std::string uniqueName(const std::string &base) {
    static std::atomic<int> sequence{0};
    return "pelican_vo4_" + base + "_" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
           "_" + std::to_string(++sequence);
}

template <class Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds limit = 20s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

OutputSpoutSender::Options realOptions(const std::string &name) {
    OutputSpoutSender::Options options;
    options.name = name;
    options.factory = Pelican::outputSpoutRealBackendFactory();
    return options;
}

// Every pixel differs by position in every channel, and alpha isn't opaque.
std::vector<std::uint8_t> pattern(std::uint32_t width, std::uint32_t height,
                                  std::uint8_t seed, bool bgra) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto red = static_cast<std::uint8_t>(seed + 10 + x);
            const auto green = static_cast<std::uint8_t>(seed + 60 + y * 3);
            const auto blue = static_cast<std::uint8_t>(seed + 120 + x * y);
            const auto alpha = static_cast<std::uint8_t>(seed + 180 + x + y);
            bytes.push_back(bgra ? blue : red);
            bytes.push_back(green);
            bytes.push_back(bgra ? red : blue);
            bytes.push_back(alpha);
        }
    }
    return bytes;
}

bool sendFrame(OutputSpoutSender &sender, const std::vector<std::uint8_t> &bytes,
               std::uint32_t width, std::uint32_t height, bool bgra) {
    auto frame = sender.acquire(width, height, bgra);
    if (!frame) return false;
    frame->pixels = bytes;
    sender.publish(std::move(*frame));
    return true;
}

// Keeps sending until the receiver's double-buffered staging shows these bytes.
// A connected ReceiveImage alone doesn't mean new pixels arrived.
bool arrives(SpoutTestReceiver &receiver, OutputSpoutSender &sender,
             const std::vector<std::uint8_t> &source, std::uint32_t width,
             std::uint32_t height, bool bgra,
             const std::vector<std::uint8_t> &expected) {
    return waitUntil([&] {
        (void)sendFrame(sender, source, width, height, bgra);
        (void)receiver.poll();
        return receiver.width() == width && receiver.height() == height &&
               receiver.pixels() == expected;
    });
}

// Device or desktop unavailable: says so as a skip.
void requireDevice(const SpoutTestReceiver &receiver) {
    if (!receiver.deviceAvailable())
        SKIP("no D3D11 device in this session: the integration test did not run");
}
} // namespace

TEST_CASE("VO-4 real Spout: channel order, orientation and alpha reach a receiver",
          "[vo4][gpu]") {
    const auto name = uniqueName("pattern");
    auto options = realOptions(name);
    SpoutTestReceiver receiver{name};
    requireDevice(receiver);
    OutputSpoutSender sender{std::move(options)};
    const auto expected = pattern(8, 4, 0, true);

    SECTION("BGRA8 source") {
        CHECK(arrives(receiver, sender, expected, 8, 4, true, expected));
    }
    SECTION("RGBA8 source") {
        CHECK(arrives(receiver, sender, pattern(8, 4, 0, false), 8, 4, false, expected));
    }
    const auto status = sender.status();
    CHECK(status.state == "running");
    CHECK(status.registered);
    CHECK(status.sender_name == name);
    CHECK(status.width == 8);
    CHECK(status.height == 4);
    CHECK(status.frames_written >= 1);
    CHECK(spoutTestSenderListed(name));
}

TEST_CASE("VO-4 real Spout: a size change arrives and status shows it", "[vo4][gpu]") {
    const auto name = uniqueName("resize");
    auto options = realOptions(name);
    SpoutTestReceiver receiver{name};
    requireDevice(receiver);
    OutputSpoutSender sender{std::move(options)};
    const auto small = pattern(4, 4, 0, true);
    REQUIRE(arrives(receiver, sender, small, 4, 4, true, small));
    const auto large = pattern(16, 2, 7, true);
    CHECK(arrives(receiver, sender, large, 16, 2, true, large));
    CHECK(sender.status().width == 16);
    CHECK(sender.status().height == 2);
    CHECK(sender.status().state == "running");
    // Back again, on the same sender.
    const auto again = pattern(4, 4, 3, true);
    CHECK(arrives(receiver, sender, again, 4, 4, true, again));
}

TEST_CASE("VO-4 real Spout: after a stop the name is gone from the sender list",
          "[vo4][gpu]") {
    const auto name = uniqueName("stop");
    auto options = realOptions(name);
    SpoutTestReceiver probe{name};
    requireDevice(probe);
    OutputSpoutSender sender{std::move(options)};
    const auto bytes = pattern(4, 4, 0, true);
    CHECK_FALSE(spoutTestSenderListed(name));
    REQUIRE(sendFrame(sender, bytes, 4, 4, true));
    REQUIRE(waitUntil([&] { return sender.status().registered; }));
    CHECK(spoutTestSenderListed(name));
    sender.requestStop();
    REQUIRE(waitUntil([&] { return sender.finished(); }));
    CHECK_FALSE(sender.status().registered);
    CHECK_FALSE(spoutTestSenderListed(name));

    // And the name can be taken again, by a sender waiting for that release.
    auto next_options = realOptions(name);
    next_options.predecessor = sender.release();
    OutputSpoutSender next{std::move(next_options)};
    REQUIRE(sendFrame(next, bytes, 4, 4, true));
    CHECK(waitUntil([&] { return next.status().registered; }));
    CHECK(next.status().sender_name == name);
}

TEST_CASE("VO-4 real Spout: a name another sender holds is refused and nothing is published",
          "[vo4][gpu]") {
    const auto name = uniqueName("taken");
    auto options = realOptions(name);
    SpoutTestSender other{name};
    if (!other.published())
        SKIP("no D3D11 device in this session: the integration test did not run");
    REQUIRE(spoutTestSenderListed(name));
    OutputSpoutSender sender{std::move(options)};
    (void)waitUntil([&] { return sender.finished(); }, 5s);
    const auto status = sender.status();
    CHECK(status.state == "refused");
    CHECK(status.last_error.rfind("output.spout_name_taken@1", 0) == 0);
    CHECK_FALSE(status.registered);
    CHECK_FALSE(sendFrame(sender, pattern(2, 2, 0, true), 2, 2, true));
    (void)waitUntil([&] { return sender.status().registered; }, 1s);
    CHECK_FALSE(spoutTestSenderListed(name + "_1"));
    // The other sender still has its name.
    CHECK(spoutTestSenderListed(name));
}

TEST_CASE("VO-4 real Spout: a missing D3D11 device refuses before any send", "[vo4][gpu]") {
    const auto name = uniqueName("nodevice");
    (void)Pelican::outputSpoutRealBackendFactory();
    OutputSpoutSender::Options options;
    options.name = name;
    options.factory = [] { return Pelican::makeOutputSpoutBackendWithoutDeviceForTesting(); };
    OutputSpoutSender sender{std::move(options)};
    (void)waitUntil([&] { return sender.status().state != "running"; }, 2s);
    const auto status = sender.status();
    CHECK(status.state == "refused");
    CHECK(status.last_error.rfind("output.spout_open_failed@1", 0) == 0);
    CHECK_FALSE(status.registered);
    CHECK_FALSE(spoutTestSenderListed(name));
}

TEST_CASE("VO-4 real Spout: a shared texture that can't be made refuses", "[vo4][gpu]") {
    // D3D11's largest texture is 16384 wide, so this size can't be created.
    const std::uint32_t too_wide = 20000;
    const auto name = uniqueName("notexture");
    auto options = realOptions(name);
    SpoutTestReceiver probe{name};
    requireDevice(probe);
    OutputSpoutSender sender{std::move(options)};
    const auto bytes = pattern(4, 4, 0, true);

    SECTION("on the first send") {
        REQUIRE(sendFrame(sender, std::vector<std::uint8_t>(too_wide * 4), too_wide, 1, true));
        const auto refused = waitUntil([&] { return sender.status().state == "refused"; }, 5s);
        CHECK(refused);
        CHECK(sender.status().last_error.rfind("output.spout_open_failed@1", 0) == 0);
        CHECK_FALSE(sender.status().registered);
        CHECK(sender.status().frames_written == 0);
    }
    SECTION("on a size change, after publishing") {
        REQUIRE(sendFrame(sender, bytes, 4, 4, true));
        REQUIRE(waitUntil([&] { return sender.status().registered; }));
        REQUIRE(sendFrame(sender, std::vector<std::uint8_t>(too_wide * 4), too_wide, 1, true));
        const auto refused = waitUntil([&] { return sender.status().state == "refused"; }, 5s);
        CHECK(refused);
        CHECK(sender.status().last_error.rfind("output.spout_open_failed@1", 0) == 0);
        // What was last accepted, and no claim to the size that failed.
        CHECK(sender.status().width == 4);
    }
    REQUIRE(waitUntil([&] { return sender.finished(); }));
    CHECK_FALSE(sender.status().registered);
    CHECK_FALSE(spoutTestSenderListed(name));
}

TEST_CASE("VO-4 real Spout: SendImage accepts a frame a held texture never shows",
          "[vo4][gpu]") {
    const auto name = uniqueName("contention");
    auto options = realOptions(name);
    SpoutTestReceiver receiver{name};
    requireDevice(receiver);
    OutputSpoutSender sender{std::move(options)};
    const auto first = pattern(4, 4, 0, true);
    REQUIRE(arrives(receiver, sender, first, 4, 4, true, first));
    // Flush both staging textures with the first frame.
    const auto settled = sender.status().frames_written;
    REQUIRE(waitUntil([&] { return sender.status().mailbox_depth == 0; }));
    for (int index = 0; index < 4; ++index) {
        const auto before = sender.status().frames_written;
        REQUIRE(sendFrame(sender, first, 4, 4, true));
        REQUIRE(waitUntil([&] { return sender.status().frames_written > before; }));
        (void)receiver.poll();
    }
    REQUIRE(receiver.pixels() == first);
    CHECK(sender.status().frames_written > settled);

    const auto second = pattern(4, 4, 90, true);
    const auto dropped = sender.status().frames_dropped;
    {
        SpoutTestTextureHold hold{name};
        REQUIRE(hold.held());
        const auto before = sender.status().frames_written;
        REQUIRE(sendFrame(sender, second, 4, 4, true));
        // The sender's wait for the mutex times out after 67 ms; it still says yes.
        const auto accepted =
            waitUntil([&] { return sender.status().frames_written == before + 1; });
        CHECK(accepted);
    }
    const auto status = sender.status();
    CHECK(status.state == "running");
    CHECK(status.frames_dropped == dropped);
    CHECK(status.last_error.empty());
    // Accepted, and not there.
    for (int index = 0; index < 8; ++index) {
        (void)receiver.poll();
        CHECK(receiver.pixels() == first);
    }
    // With the mutex free the next frames arrive, so the sender wasn't harmed.
    CHECK(arrives(receiver, sender, second, 4, 4, true, second));
}
