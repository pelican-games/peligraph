#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/os/liveinputs.hpp"
#include "../src/core/os/liveinputrecording.hpp"
#include "../src/core/osc/oscbindings.hpp"
#include "../src/core/osc/oscinput.hpp"
#include "../src/core/osc/oscparser.hpp"
#include "../src/core/userpublic/userinput.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Pelican;
using Catch::Matchers::StartsWith;

namespace {

using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;

constexpr std::uint64_t ntp_offset = 2208988800ULL;
constexpr std::string_view liveDeclaration = R"({
  "schema": "pelican.live_inputs", "version": 1,
  "inputs": [
    {"name": "level", "type": "float", "default": 0.0, "min": 0.0, "max": 1.0},
    {"name": "mapped", "type": "float", "default": 0.0},
    {"name": "signed_int", "type": "int", "default": 0, "min": -100, "max": 100},
    {"name": "count", "type": "int", "default": 0, "min": 0, "max": 10},
    {"name": "enabled", "type": "bool", "default": false}
  ]
})";

void putU32(Bytes &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void putI32(Bytes &out, std::int32_t value) {
    putU32(out, std::bit_cast<std::uint32_t>(value));
}

void putU64(Bytes &out, std::uint64_t value) {
    putU32(out, static_cast<std::uint32_t>(value >> 32));
    putU32(out, static_cast<std::uint32_t>(value));
}

void putF32(Bytes &out, float value) {
    putU32(out, std::bit_cast<std::uint32_t>(value));
}

void putString(Bytes &out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
    while ((out.size() % 4) != 0) out.push_back(0);
}

Bytes makeMessage(std::string_view address, std::string_view tags,
                  std::initializer_list<Bytes> arguments = {}) {
    Bytes out;
    putString(out, address);
    if (!tags.empty()) putString(out, tags);
    for (const auto &argument : arguments)
        out.insert(out.end(), argument.begin(), argument.end());
    return out;
}

Bytes f32(float value) {
    Bytes out;
    putF32(out, value);
    return out;
}

Bytes i32(std::int32_t value) {
    Bytes out;
    putI32(out, value);
    return out;
}

Bytes i64(std::int64_t value) {
    Bytes out;
    putU64(out, static_cast<std::uint64_t>(value));
    return out;
}

Bytes f64(double value) {
    Bytes out;
    putU64(out, std::bit_cast<std::uint64_t>(value));
    return out;
}

Bytes makeBundle(std::uint64_t timetag, const std::vector<Bytes> &elements) {
    Bytes out{'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
    putU64(out, timetag);
    for (const auto &element : elements) {
        putI32(out, static_cast<std::int32_t>(element.size()));
        out.insert(out.end(), element.begin(), element.end());
    }
    return out;
}

Bytes rawBundle(std::uint64_t timetag, const Bytes &tail) {
    Bytes out{'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
    putU64(out, timetag);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

Osc::Packet parse(const Bytes &bytes, std::uint64_t = 1ULL << 40) {
    return Osc::parsePacket(std::span<const std::uint8_t>{bytes});
}

std::vector<Osc::OscBinding> bindings(Json profile) {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(liveDeclaration);
    return Osc::parseOscBindings(profile, live.declarations());
}

void writeText(const std::filesystem::path &path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file{path, std::ios_base::binary};
    file << text;
}

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

struct Sandbox {
    std::filesystem::path base;
    std::filesystem::path root;

    Sandbox() {
        const auto suffix =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        base = std::filesystem::temp_directory_path() /
               ("pelican_osc2a_" + suffix);
        root = base / "project";
    }

    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios_base::binary};
    return std::string{std::istreambuf_iterator<char>{file},
                       std::istreambuf_iterator<char>{}};
}

std::uint64_t ntp(std::uint32_t seconds, std::uint32_t fraction = 0) {
    return (static_cast<std::uint64_t>(seconds) << 32) | fraction;
}

} // namespace

TEST_CASE("OSC-2a parser accepts valid messages and bundles and decodes arguments",
          "[osc2a][osc][parser]") {
    const auto now = ntp(1000);

    SECTION("message payloads are big-endian and typed") {
        const auto int_message = makeMessage("/i", ",i", {i32(-7)});
        auto packet = parse(int_message, now);
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE_FALSE(packet.bundle);
        REQUIRE(packet.messages.size() == 1);
        REQUIRE(packet.messages.front().address == "/i");
        REQUIRE(packet.messages.front().reject == Osc::MessageReject::none);
        REQUIRE(packet.messages.front().arguments.size() == 1);
        REQUIRE(packet.messages.front().arguments.front().tag == 'i');
        REQUIRE(packet.messages.front().arguments.front().integer == -7);

        const auto float_message = makeMessage("/f", ",f", {f32(0.25f)});
        packet = parse(float_message, now);
        REQUIRE(packet.messages.front().arguments.front().tag == 'f');
        REQUIRE(packet.messages.front().arguments.front().number ==
                Catch::Approx(0.25));

        const auto double_message = makeMessage("/d", ",d", {f64(0.125)});
        packet = parse(double_message, now);
        REQUIRE(packet.messages.front().arguments.front().number ==
                Catch::Approx(0.125));

        const auto text_message = makeMessage("/s", ",s", {});
        // Append the string payload by replacing the empty argument list.
        Bytes with_string;
        putString(with_string, "/s");
        putString(with_string, ",s");
        putString(with_string, "hello");
        packet = parse(with_string, now);
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.messages.front().arguments.front().text == "hello");

        const auto blob_message = makeMessage("/b", ",b", {i32(4)});
        Bytes with_blob = blob_message;
        with_blob.insert(with_blob.end(), {1, 2, 3, 4});
        packet = parse(with_blob, now);
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.messages.front().arguments.front().blob ==
                Bytes{1, 2, 3, 4});

        // Blob length counts data bytes; the final 0-3 bytes are zero padding.
        Bytes padded_blob;
        putString(padded_blob, "/b");
        putString(padded_blob, ",b");
        putI32(padded_blob, 5);
        padded_blob.insert(padded_blob.end(), {1, 2, 3, 4, 5, 0, 0, 0});
        packet = parse(padded_blob, now);
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.messages.front().arguments.front().blob ==
                Bytes{1, 2, 3, 4, 5});
    }

    SECTION("T and F consume no payload; TF is two arguments") {
        auto packet = parse(makeMessage("/t", ",T"), now);
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.messages.front().arguments.size() == 1);
        REQUIRE(packet.messages.front().arguments.front().boolean);

        packet = parse(makeMessage("/f", ",F"), now);
        REQUIRE(packet.messages.front().arguments.size() == 1);
        REQUIRE_FALSE(packet.messages.front().arguments.front().boolean);

        packet = parse(makeMessage("/tf", ",TF"), now);
        REQUIRE(packet.messages.front().arguments.size() == 2);
        REQUIRE(packet.messages.front().arguments[0].boolean);
        REQUIRE_FALSE(packet.messages.front().arguments[1].boolean);
    }

    SECTION("an empty bundle has a timetag and no messages") {
        auto packet = parse(makeBundle(ntp(999), {}), now);
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.bundle);
        REQUIRE(packet.timetag == ntp(999));
        REQUIRE(packet.messages.empty());
    }

    SECTION("future timetags remain in the parsed packet") {
        const auto packet = parse(
            makeBundle(ntp(1001), {makeMessage("/x", ",i", {i32(1)})}),
            ntp(1000));
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.timetag == ntp(1001));
        REQUIRE(packet.messages.size() == 1);
    }

    SECTION("NTP helper is UTC in 32.32 fixed point") {
        // Sample the clock once on each side so a second boundary between the
        // helper and the reference clock cannot make the check flaky.
        const auto before = ntpNowUtc();
        const auto wall = std::chrono::system_clock::now().time_since_epoch();
        const auto after = ntpNowUtc();
        const auto expected =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(wall).count()) +
            ntp_offset;
        CHECK((before >> 32) <= expected);
        CHECK((after >> 32) >= expected);
        CHECK((after >> 32) - (before >> 32) <= 1);
        CHECK((before & 0xFFFFFFFFULL) != 0);
    }
}

TEST_CASE("OSC-2a parser is strict about lengths, strings, element sizes and exact ends",
          "[osc2a][osc][parser][negative]") {
    const auto now = ntp(1000);

    SECTION("packet length must be a multiple of four and non-zero") {
        Bytes bytes{0, 0, 0};
        REQUIRE(parse(bytes, now).drop == Osc::PacketDrop::malformed);
        REQUIRE(parse(Bytes{}, now).drop == Osc::PacketDrop::malformed);
    }

    SECTION("strings need printability, NUL termination and zero padding") {
        Bytes address = makeMessage("/ok", ",i", {i32(1)});
        address[1] = 0x01;
        REQUIRE(parse(address, now).drop == Osc::PacketDrop::malformed);

        Bytes unterminated{'/', 'a', 'b', 'c'};
        REQUIRE(parse(unterminated, now).drop == Osc::PacketDrop::malformed);

        Bytes bad_padding = makeMessage("/a", ",T");
        bad_padding.back() = 1;
        REQUIRE(parse(bad_padding, now).drop == Osc::PacketDrop::malformed);
    }

    SECTION("a message needs a type tag string and exact blob padding") {
        Bytes address_only;
        putString(address_only, "/lonely");
        REQUIRE(parse(address_only, now).drop == Osc::PacketDrop::malformed);

        Bytes bad_blob;
        putString(bad_blob, "/b");
        putString(bad_blob, ",b");
        putI32(bad_blob, 5);
        bad_blob.insert(bad_blob.end(), {1, 2, 3, 4, 5, 0, 1, 0});
        REQUIRE(parse(bad_blob, now).drop == Osc::PacketDrop::malformed);

        // A negative blob length is rejected while it is still signed. If it
        // were read as unsigned, adding the 3 padding bytes would wrap to zero
        // and the empty remainder would look valid.
        Bytes negative_blob;
        putString(negative_blob, "/b");
        putString(negative_blob, ",b");
        putI32(negative_blob, -1);
        REQUIRE(parse(negative_blob, now).drop == Osc::PacketDrop::malformed);
    }

    SECTION("the message must end exactly at the last argument") {
        auto bytes = makeMessage("/a", ",i", {i32(1)});
        bytes.insert(bytes.end(), {0, 0, 0, 0});
        REQUIRE(parse(bytes, now).drop == Osc::PacketDrop::malformed);
    }

    SECTION("bundle element sizes are positive signed multiples of four in range") {
        for (const std::int32_t size : {0, -4, 6, 0x7ffffffc}) {
            Bytes tail;
            putI32(tail, size);
            tail.insert(tail.end(), {0, 0, 0, 0});
            REQUIRE(parse(rawBundle(0, tail), now).drop ==
                    Osc::PacketDrop::malformed);
        }
    }

    SECTION("damage anywhere rejects the whole packet including earlier messages") {
        Bytes tail;
        putI32(tail, static_cast<std::int32_t>(makeMessage("/good", ",i", {i32(1)}).size()));
        const auto good = makeMessage("/good", ",i", {i32(1)});
        tail.insert(tail.end(), good.begin(), good.end());
        putI32(tail, 0);
        tail.insert(tail.end(), {0, 0, 0, 0});
        auto packet = parse(rawBundle(0, tail), now);
        REQUIRE(packet.drop == Osc::PacketDrop::malformed);
        REQUIRE(packet.messages.empty());
    }

    SECTION("a structurally valid bundle rejects only unsupported, pattern and nested elements") {
        const auto nested = makeBundle(ntp(999), {makeMessage("/in", ",i", {i32(3)})});
        const auto packet = parse(
            makeBundle(ntp(999),
                       {makeMessage("/good", ",i", {i32(1)}),
                        makeMessage("/n", ",N"),
                        makeMessage("/w*", ",i", {i32(1)}), nested}),
            now);
        REQUIRE(packet.drop == Osc::PacketDrop::none);
        REQUIRE(packet.messages.size() == 4);
        REQUIRE(packet.messages[0].reject == Osc::MessageReject::none);
        REQUIRE(packet.messages[1].reject == Osc::MessageReject::unsupported_type);
        REQUIRE(packet.messages[2].reject == Osc::MessageReject::pattern_unsupported);
        REQUIRE(packet.messages[3].reject == Osc::MessageReject::nested_bundle);
        REQUIRE(packet.messages[3].address == "#bundle");
    }

    SECTION("too many bundle elements is a limit, not corruption") {
        std::vector<Bytes> elements;
        for (std::size_t i = 0; i < Osc::max_messages_per_packet + 1; ++i)
            elements.push_back(makeMessage("/x", ",i", {i32(1)}));
        REQUIRE(parse(makeBundle(0, elements), now).drop ==
                Osc::PacketDrop::limit);
    }
}

TEST_CASE("OSC-2a bindings validate addresses, live names, maps and unknown keys",
          "[osc2a][osc][bindings]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(liveDeclaration);
    const auto declarations = live.declarations();

    const auto parse_json = [&](const Json &profile) {
        try {
            (void)Osc::parseOscBindings(profile, declarations);
            return std::string{};
        } catch (const std::runtime_error &error) {
            return std::string{error.what()};
        }
    };

    const auto valid = Json{
        {"live",
         Json::array(
             {Json{{"address", "/level"}, {"input", "level"}},
              Json{{"address", "/mapped"},
                   {"input", "mapped"},
                   {"map", {{"from", {-1.0, 1.0}}, {"to", {0.0, 100.0}}}}}})}};
    const auto parsed = Osc::parseOscBindings(valid, declarations);
    REQUIRE(parsed.size() == 2);
    REQUIRE(parsed[0].input_type == LiveInputType::floating);
    REQUIRE(parsed[1].has_map);
    REQUIRE(parsed[1].map.to[1] == Catch::Approx(100.0));

    for (const auto &bad :
         {Json{{"live", Json::array({Json{{"address", "/x"}, {"input", "nope"}}})}},
          Json{{"live", Json::array({Json{{"address", "x"}, {"input", "level"}}})}},
          Json{{"live", Json::array({Json{{"address", "/x*"}, {"input", "level"}}})}},
          Json{{"live", Json::array({Json{{"address", "/x"}, {"input", "level"}, {"smooth", true}}})}},
          Json{{"live", Json::array({Json{{"address", "/x"}, {"input", "level"}, {"map", {{"from", {0.0, 1.0}}, {"to", {0.0, 1.0}}, {"smooth", 1}}}},})}},
          Json{{"live", Json::array({Json{{"address", "/x"}, {"input", "level"}, {"map", {{"from", {1.0, 1.0}}, {"to", {0.0, 1.0}}}}}})}},
          Json{{"live", Json::array({Json{{"address", "/x"}, {"input", "enabled"}, {"map", {{"from", {0.0, 1.0}}, {"to", {0.0, 1.0}}}}}})}},
          Json{{"live", Json::array({Json{{"address", "/x"}, {"input", "level"}}, Json{{"address", "/x"}, {"input", "mapped"}}})}},
          Json{{"live", Json::array({Json{{"address", "/x"}, {"input", "level"}, {"map", {{"from", {0.0, 1.0}}}}}})}}}) {
        const auto message = parse_json(bad);
        INFO(message);
        REQUIRE_THAT(message, StartsWith("osc.binding_invalid@1:"));
    }
}

TEST_CASE("OSC-2c receive takes exactly one f/i/T/F argument and drops the rest",
          "[osc2c][osc][receive]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(liveDeclaration);
    live.freeze(false, 1);

    OscInput osc;
    osc.setBindings(bindings(Json{
        {"live",
         Json::array(
             {Json{{"address", "/level"}, {"input", "level"}},
              Json{{"address", "/mapped"},
                   {"input", "mapped"},
                   {"map", {{"from", {-1.0, 1.0}}, {"to", {0.0, 100.0}}}}},
              Json{{"address", "/overflow"},
                   {"input", "mapped"},
                   {"map", {{"from", {0.0, 1.0}},
                            {"to", {-1.0e308, 1.0e308}}}}},
              Json{{"address", "/signed"},
                   {"input", "signed_int"},
                   {"map", {{"from", {-10.0, 10.0}}, {"to", {-10.0, 10.0}}}}},
              Json{{"address", "/count"}, {"input", "count"}},
              Json{{"address", "/enabled"}, {"input", "enabled"}}})}}));

    osc.receive(makeMessage("/level", ",f", {f32(0.5f)}), ntp(1000));
    // Staging is not applying: the frozen value only moves at freeze.
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.0));
    live.freeze(false, 2);
    auto snapshot = live.snapshot();
    REQUIRE(snapshot[0].value == Catch::Approx(0.5));
    REQUIRE(snapshot[0].source == LiveInputSource::osc);

    // Exactly one argument: a second argument makes the message ambiguous,
    // so nothing is consumed and the stamped value does not move.
    osc.receive(makeMessage("/level", ",ff", {f32(0.2f), f32(0.9f)}), ntp(1001));
    live.freeze(false, 3);
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.5));
    REQUIRE(osc.snapshotJson().at("dropped").at("argument_count") == 1);

    // No argument is equally ambiguous.
    osc.receive(makeMessage("/level", ",", {}), ntp(1002));
    live.freeze(false, 4);
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.5));
    REQUIRE(osc.snapshotJson().at("dropped").at("argument_count") == 2);

    // d/h/t/c/r/m are structurally parsed but have no live destination here.
    const std::vector<std::pair<std::string, Bytes>> unsupported = {
        {"d", f64(0.75)},
        {"h", i64(7)},
        {"t", i64(8)},
        {"c", i32(65)},
        {"r", i32(0x11223344)},
        {"m", i32(0x0090337f)},
    };
    std::uint64_t now = 1100;
    for (const auto &[tag, payload] : unsupported) {
        osc.receive(makeMessage("/level", "," + tag, {payload}), ntp(now++));
    }
    live.freeze(false, 4);
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.5));
    REQUIRE(osc.snapshotJson().at("dropped").at("unsupported_type") == 6);

    // One unsupported element does not discard the rest of the bundle.
    osc.receive(makeBundle(ntp(999),
                           {makeMessage("/level", ",d", {f64(0.9)}),
                            makeMessage("/level", ",f", {f32(0.3f)})}),
                ntp(now++));
    live.freeze(false, 5);
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.3));

    // The structural walk still consumes the d payload, so a truncated
    // argument after it is malformed rather than silently accepted.
    auto truncated = makeMessage("/level", ",di", {f64(0.5), i32(1)});
    truncated.resize(truncated.size() - 4);
    REQUIRE(parse(truncated).drop == Osc::PacketDrop::malformed);

    // Reversed and non-unit ranges are legal; clamp still applies afterwards.
    osc.receive(makeMessage("/mapped", ",f", {f32(-0.5f)}), ntp(now++));
    live.freeze(false, 6);
    REQUIRE(live.snapshot()[1].value == Catch::Approx(25.0));

    // -1.5 goes through round, then int32 range, then the declared clamp.
    osc.receive(makeMessage("/signed", ",f", {f32(-1.5f)}), ntp(now++));
    live.freeze(false, 7);
    REQUIRE(live.snapshot()[2].value == Catch::Approx(-2.0));

    osc.receive(makeMessage("/count", ",i", {i32(99)}), ntp(now++));
    live.freeze(false, 8);
    snapshot = live.snapshot();
    REQUIRE(snapshot[3].value == Catch::Approx(10.0));
    REQUIRE(snapshot[3].clamped);

    osc.receive(makeMessage("/enabled", ",T"), ntp(now++));
    live.freeze(false, 9);
    REQUIRE(live.snapshot()[4].value == Catch::Approx(1.0));

    osc.receive(makeMessage("/enabled", ",i", {i32(2)}), ntp(now++));
    live.freeze(false, 10);
    REQUIRE(live.snapshot()[4].value == Catch::Approx(1.0));
    osc.receive(makeMessage("/enabled", ",F"), ntp(now++));
    live.freeze(false, 11);
    REQUIRE(live.snapshot()[4].value == Catch::Approx(0.0));

    // T/F are only meaningful for a bool binding.
    osc.receive(makeMessage("/level", ",T"), ntp(now++));
    // A 64-bit integer is outside the accepted tag set, not merely out of
    // int32 range, and never reaches the live input.
    osc.receive(makeMessage("/signed", ",h", {i64(std::int64_t{1} << 40)}),
                ntp(now++));
    osc.receive(makeMessage("/level", ",f",
                            {f32(std::numeric_limits<float>::quiet_NaN())}),
                ntp(now++));
    // A finite receiver value can still map out of double range; the mapped
    // result is re-checked after extrapolation.
    osc.receive(makeMessage("/overflow", ",f", {f32(0.5f)}), ntp(now++));
    // A finite float can still be too large for an int32 live input.
    osc.receive(makeMessage("/count", ",f", {f32(3.0e38f)}), ntp(now++));
    // Bool inputs still refuse a non-finite numeric argument rather than
    // turning NaN into true.
    osc.receive(makeMessage("/enabled", ",f",
                            {f32(std::numeric_limits<float>::quiet_NaN())}),
                ntp(now++));
    live.freeze(false, 12);
    REQUIRE(live.snapshot()[2].value == Catch::Approx(-2.0));
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.3));
    REQUIRE(live.snapshot()[1].value == Catch::Approx(25.0));
    REQUIRE(live.snapshot()[4].value == Catch::Approx(0.0));

    const auto counters = osc.snapshotJson();
    REQUIRE(counters.at("received") == 22);
    REQUIRE(counters.at("applied") == 8);
    REQUIRE(counters.at("dropped").at("argument_count") == 2);
    REQUIRE(counters.at("dropped").at("unsupported_type") == 9);
    REQUIRE(counters.at("dropped").at("out_of_range") == 1);
    REQUIRE(counters.at("dropped").at("non_finite") == 3);
}

TEST_CASE("OSC-2a counters, recent records and replay are observable as one snapshot",
          "[osc2a][osc][counters][replay]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(liveDeclaration);
    live.freeze(false, 1);

    OscInput osc;
    osc.setBindings(
        bindings(Json{{"live", Json::array({Json{{"address", "/level"},
                                                {"input", "level"}}})}}));

    osc.receive(makeMessage("/unknown", ",i", {i32(1)}), ntp(1000));
    osc.receive(makeMessage("/level", ",N"), ntp(1000));
    osc.receive(makeMessage("/level", ","), ntp(1000));
    osc.receive(Bytes{0, 0, 0}, ntp(1000));
    auto counters = osc.snapshotJson();
    REQUIRE(counters.at("received") == 4);
    REQUIRE(counters.at("applied") == 0);
    REQUIRE(counters.at("unbound") == 1);
    REQUIRE(counters.at("dropped").at("unsupported_type") == 1);
    REQUIRE(counters.at("dropped").at("argument_count") == 1);
    REQUIRE(counters.at("dropped").at("malformed") == 1);
    REQUIRE(counters.at("recent").size() == 3);
    REQUIRE(counters.at("recent")[0].at("bound") == false);
    REQUIRE(counters.at("recent")[0].at("reason") == "unbound");
    REQUIRE(counters.at("recent")[1].at("bound") == true);
    REQUIRE(counters.at("recent")[1].at("input") == "level");

    // Replay refuses the whole batch without throwing and records it.
    live.freeze(true, 2);
    osc.receive(makeMessage("/level", ",f", {f32(0.75f)}), ntp(1001));
    counters = osc.snapshotJson();
    REQUIRE(counters.at("dropped").at("replay") == 1);
    REQUIRE(counters.at("recent").back().at("reason") == "replay");
    live.freeze(false, 3);
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.0));

    // A bundle too far ahead is a packet drop; a valid bundle commits together.
    osc.receive(makeBundle(ntp(5000),
                           {makeMessage("/level", ",f", {f32(0.4f)})}),
                ntp(1000));
    REQUIRE(osc.snapshotJson().at("dropped").at("future_timetag") == 1);

    osc.receive(makeBundle(ntp(999),
                           {makeMessage("/level", ",f", {f32(0.4f)}),
                            makeMessage("/missing", ",i", {i32(1)})}),
                ntp(1000));
    live.freeze(false, 4);
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.4));
    counters = osc.snapshotJson();
    REQUIRE(counters.at("recent").back().at("reason") == "unbound");

    // The recent ring never grows past its fixed capacity.
    for (std::size_t i = 0; i < 100; ++i)
        osc.receive(makeMessage("/unbound", ",i", {i32(1)}), ntp(1000));
    REQUIRE(osc.snapshotJson().at("recent").size() == 64);
}

TEST_CASE("OSC-2a OSC and RPC writes share the source field and the newest one wins",
          "[osc2a][osc][source]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(liveDeclaration);
    live.freeze(false, 1);

    OscInput osc;
    osc.setBindings(
        bindings(Json{{"live", Json::array({Json{{"address", "/level"},
                                                {"input", "level"}}})}}));

    REQUIRE(live.stageValues({{"level", 0.5}}) == 1);
    live.freeze(false, 2);
    REQUIRE(live.snapshot()[0].source == LiveInputSource::rpc);
    REQUIRE(std::string{liveInputSourceName(live.snapshot()[0].source)} == "rpc");

    // A numerically identical OSC value still becomes the source of record.
    osc.receive(makeMessage("/level", ",f", {f32(0.5f)}), ntp(1000));
    live.freeze(false, 3);
    REQUIRE(live.snapshot()[0].source == LiveInputSource::osc);
    REQUIRE(live.snapshot()[0].changed_frame == 2);
    REQUIRE(std::string{liveInputSourceName(live.snapshot()[0].source)} == "osc");
}

TEST_CASE("OSC-2a profile loading applies live bindings and a profile without live clears them",
          "[osc2a][osc][profile]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "actions.json",
              R"({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"gameplay","actions":[{"name":"jump","type":"button"}]}]})");
    writeText(sandbox.root / "input" / "live.json", liveDeclaration);
    writeText(sandbox.root / "input" / "profiles" / "live.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"live","bindings":[],"live":[{"address":"/level","input":"level"}]})");
    writeText(sandbox.root / "input" / "profiles" / "plain.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"plain","bindings":[]})");
    const auto project = Json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "osc-profile"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"input_actions_json", "input/actions.json"},
          {"live_inputs_json", "input/live.json"},
          {"input_profiles",
           {{"live", "input/profiles/live.json"},
            {"plain", "input/profiles/plain.json"}}},
          {"input_profile", "live"}}},
    };
    const auto project_bytes = project.dump();
    writeText(sandbox.root / "project.json", project_bytes);

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project_bytes);
    REQUIRE(Actions::isConfigured());
    REQUIRE(internal::activeInputProfile() ==
            std::optional<std::string>{"live"});

    auto &osc = GET_MODULE(OscInput);
    osc.receive(makeMessage("/level", ",f", {f32(0.5f)}), ntp(1000));
    GET_MODULE(LiveInputs).freeze(false, 2);
    REQUIRE(GET_MODULE(LiveInputs).snapshot()[0].value == Catch::Approx(0.5));

    // Switching to a profile without a live member removes the old table.
    internal::selectInputProfile("plain");
    const auto before_switch = GET_MODULE(LiveInputs).snapshot()[0].value;
    osc.receive(makeMessage("/level", ",f", {f32(0.9f)}), ntp(1001));
    GET_MODULE(LiveInputs).freeze(false, 3);
    REQUIRE(GET_MODULE(LiveInputs).snapshot()[0].value ==
            Catch::Approx(before_switch));
    REQUIRE(osc.snapshotJson().at("recent").back().at("reason") == "unbound");

    // Switching back restores the bindings.
    internal::selectInputProfile("live");
    osc.receive(makeMessage("/level", ",f", {f32(0.25f)}), ntp(1002));
    GET_MODULE(LiveInputs).freeze(false, 4);
    REQUIRE(GET_MODULE(LiveInputs).snapshot()[0].value ==
            Catch::Approx(0.25));
    REQUIRE(readText(sandbox.root / "project.json") == project_bytes);
}

TEST_CASE("OSC-2c a project without actions still loads and selects live profiles",
          "[osc2c][osc][profile]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "live.json", liveDeclaration);
    writeText(sandbox.root / "input" / "profiles" / "live.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"live","bindings":[],"live":[{"address":"/level","input":"level"}]})");
    writeText(sandbox.root / "input" / "profiles" / "plain.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"plain","bindings":[]})");
    const auto project = Json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "osc-no-actions"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"live_inputs_json", "input/live.json"},
          {"input_profiles",
           {{"live", "input/profiles/live.json"},
            {"plain", "input/profiles/plain.json"}}},
          {"input_profile", "plain"}}},
    };
    const auto project_bytes = project.dump();
    writeText(sandbox.root / "project.json", project_bytes);

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project_bytes);
    // --input-profile overrides the project's default selection.
    GET_MODULE(EngineLaunchConfig).input_profile = "live";

    // No action document means no action map, but the live-only profile is
    // still a real profile with a real OSC table.
    REQUIRE_FALSE(Actions::isConfigured());
    REQUIRE(internal::activeInputProfile() ==
            std::optional<std::string>{"live"});
    auto &osc = GET_MODULE(OscInput);
    REQUIRE(osc.snapshotJson().at("bindings") == 1);

    osc.receive(makeMessage("/level", ",f", {f32(0.5f)}), ntp(1000));
    GET_MODULE(LiveInputs).freeze(false, 2);
    REQUIRE(GET_MODULE(LiveInputs).snapshot()[0].value == Catch::Approx(0.5));

    // A profile without a live member still switches and clears the table.
    internal::selectInputProfile("plain");
    REQUIRE(internal::activeInputProfile() ==
            std::optional<std::string>{"plain"});
    REQUIRE(osc.snapshotJson().at("bindings") == 0);
    const auto before_switch = GET_MODULE(LiveInputs).snapshot()[0].value;
    osc.receive(makeMessage("/level", ",f", {f32(0.9f)}), ntp(1001));
    GET_MODULE(LiveInputs).freeze(false, 3);
    REQUIRE(GET_MODULE(LiveInputs).snapshot()[0].value ==
            Catch::Approx(before_switch));
    REQUIRE(osc.snapshotJson().at("recent").back().at("reason") == "unbound");

    // set_input_profile / --input-profile can select the live-only profile.
    internal::selectInputProfile("live");
    REQUIRE(osc.snapshotJson().at("bindings") == 1);
    osc.receive(makeMessage("/level", ",f", {f32(0.25f)}), ntp(1002));
    GET_MODULE(LiveInputs).freeze(false, 4);
    REQUIRE(GET_MODULE(LiveInputs).snapshot()[0].value == Catch::Approx(0.25));
    REQUIRE(readText(sandbox.root / "project.json") == project_bytes);
}

TEST_CASE("OSC-2c a profile with action bindings needs the action document",
          "[osc2c][osc][profile][negative]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "live.json", liveDeclaration);
    writeText(sandbox.root / "input" / "profiles" / "needsactions.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"needsactions","bindings":[{"action":"jump","binding":"kbd:space"}],"live":[]})");
    const auto project = Json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "osc-requires-actions"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"live_inputs_json", "input/live.json"},
          {"input_profiles",
           {{"needsactions", "input/profiles/needsactions.json"}}},
          {"input_profile", "needsactions"}}},
    };
    const auto project_bytes = project.dump();

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project_bytes);
    REQUIRE_THROWS_WITH(
        Actions::isConfigured(),
        StartsWith("input.profile_requires_actions@1: input profile "
                   "'needsactions'"));
}

TEST_CASE("OSC-2a a bad live table is refused before either half of the profile applies",
          "[osc2a][osc][profile][negative]") {
    ensureLogger();
    Sandbox sandbox;
    writeText(sandbox.root / "input" / "actions.json",
              R"({"schema":"pelican.input_actions","version":1,"action_sets":[{"name":"gameplay","actions":[{"name":"jump","type":"button"}]}]})");
    writeText(sandbox.root / "input" / "live.json", liveDeclaration);
    writeText(sandbox.root / "input" / "profiles" / "bad.json",
              R"({"schema":"pelican.input_profile","version":1,"name":"bad","bindings":[],"live":[{"address":"/level","input":"missing"}]})");
    const auto project = Json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "osc-bad-profile"},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"input_actions_json", "input/actions.json"},
          {"live_inputs_json", "input/live.json"},
          {"input_profiles", {{"bad", "input/profiles/bad.json"}}},
          {"input_profile", "bad"}}},
    };
    const auto project_bytes = project.dump();

    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(sandbox.root, false);
    GET_MODULE(ProjectSource).setProjectData(project_bytes);
    REQUIRE_THROWS_WITH(Actions::isConfigured(),
                        StartsWith("osc.binding_invalid@1:"));
}

TEST_CASE("OSC-2a one mutex survives concurrent profile switching and packet input",
          "[osc2a][osc][threads]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(liveDeclaration);
    live.freeze(false, 1);

    OscInput osc;
    const auto table_a = bindings(
        Json{{"live", Json::array({Json{{"address", "/level"}, {"input", "level"}}})}});
    const auto table_b = bindings(
        Json{{"live", Json::array({Json{{"address", "/level"}, {"input", "mapped"}}})}});
    osc.setBindings(table_a);

    std::atomic_bool stop{false};
    std::atomic<std::uint64_t> sends{0};
    std::thread switcher([&] {
        for (std::size_t i = 0; i < 2000; ++i) {
            osc.setBindings((i & 1) != 0 ? table_b : table_a);
        }
        stop.store(true, std::memory_order_release);
    });
    std::thread receiver([&] {
        const auto message = makeMessage("/level", ",f", {f32(0.5f)});
        while (!stop.load(std::memory_order_acquire)) {
            osc.receive(message, ntp(1000), "thread");
            sends.fetch_add(1, std::memory_order_relaxed);
        }
    });
    switcher.join();
    receiver.join();
    REQUIRE(sends.load() > 0);
    const auto snapshot = osc.snapshotJson();
    REQUIRE(snapshot.at("received") == sends.load());
    REQUIRE(snapshot.at("applied").get<std::uint64_t>() +
                snapshot.at("dropped").at("replay").get<std::uint64_t>() +
                snapshot.at("unbound").get<std::uint64_t>() ==
            sends.load());
}

TEST_CASE("OSC-2a fixed-seed fuzzing reaches valid messages and bundles without escaping",
          "[osc2a][osc][fuzz]") {
    std::mt19937 rng(0x05C2A);
    const auto now = ntp(1000);
    std::size_t valid_messages = 0;
    std::size_t valid_bundles = 0;
    std::size_t decoded_numbers = 0;

    const auto randomBytes = [&](std::size_t count) {
        Bytes bytes(count);
        for (auto &byte : bytes)
            byte = static_cast<std::uint8_t>(rng());
        return bytes;
    };

    for (std::size_t iteration = 0; iteration < 10000; ++iteration) {
        Bytes packet;
        const auto address = "/fuzz" + std::to_string(iteration % 16);
        switch (iteration % 6) {
        case 0:
            packet = makeMessage(address, ",f", {f32(0.5f)});
            break;
        case 1:
            packet = makeMessage(address, ",i", {i32(-3)});
            break;
        case 2:
            packet = makeMessage(address, ",s", {});
            putString(packet, "text");
            break;
        case 3:
            packet = makeMessage(address, ",TF");
            break;
        case 4:
            packet = makeBundle(ntp(999),
                                {makeMessage(address, ",d", {f64(0.25)})});
            break;
        case 5:
            packet = makeBundle(ntp(999), {});
            break;
        }
        if ((iteration & 1) != 0) {
            packet[static_cast<std::size_t>(rng()) % packet.size()] ^=
                static_cast<std::uint8_t>(1u << (rng() % 8));
        } else {
            const auto padding = randomBytes(4 * (rng() % 3));
            packet.insert(packet.end(), padding.begin(), padding.end());
        }
        const auto parsed = parse(packet, now);
        if (parsed.drop == Osc::PacketDrop::none) {
            if (parsed.bundle) ++valid_bundles;
            for (const auto &message : parsed.messages) {
                if (message.reject == Osc::MessageReject::none &&
                    !message.address.empty())
                    ++valid_messages;
                for (const auto &argument : message.arguments)
                    if (argument.tag == 'f' || argument.tag == 'd')
                        ++decoded_numbers;
            }
        }
    }

    for (std::size_t iteration = 0; iteration < 10000; ++iteration) {
        auto packet = randomBytes(4 * (rng() % 64));
        if ((iteration & 3) == 0 && packet.size() >= 16) {
            const std::array<std::uint8_t, 8> header{'#', 'b', 'u', 'n',
                                                     'd', 'l', 'e', 0};
            std::copy(header.begin(), header.end(), packet.begin());
        }
        const auto parsed = parse(packet, now);
        if (parsed.drop == Osc::PacketDrop::none) {
            if (parsed.bundle) ++valid_bundles;
            for (const auto &message : parsed.messages) {
                if (message.reject == Osc::MessageReject::none &&
                    !message.address.empty())
                    ++valid_messages;
                for (const auto &argument : message.arguments)
                    if (argument.tag == 'f' || argument.tag == 'd')
                        ++decoded_numbers;
            }
        }
    }

    // A deep bundle whose innermost element size is invalid must be dropped,
    // not trusted into a recursive decode.
    Bytes deep = rawBundle(0, {});
    Bytes bad_tail;
    putI32(bad_tail, 0);
    bad_tail.insert(bad_tail.end(), {0, 0, 0, 0});
    deep = rawBundle(0, bad_tail);
    for (int depth = 0; depth < 8; ++depth) deep = makeBundle(0, {deep});
    const auto deep_packet = parse(deep, now);
    REQUIRE((deep_packet.drop == Osc::PacketDrop::malformed ||
             deep_packet.drop == Osc::PacketDrop::limit));

    CAPTURE(valid_messages, valid_bundles, decoded_numbers);
    REQUIRE(valid_messages > 0);
    REQUIRE(valid_bundles > 0);
    REQUIRE(decoded_numbers > 0);
}


namespace {
struct Osc5aFixture {
    OscInput osc;
    LiveInputs &live = GET_MODULE(LiveInputs);

    Osc5aFixture() {
        if (live.isLiveReplaying()) live.endLiveReplay();
        live.setDeclarationJson(liveDeclaration);
        live.freeze(false, 1);
        osc.setBindings({
            {"/level", "level", LiveInputType::floating, false, {}},
            {"/count", "count", LiveInputType::integer, false, {}},
        });
    }

    Bytes level(float value) const {
        return makeMessage("/level", ",f", {f32(value)});
    }
    Bytes count(std::int32_t value) const {
        return makeMessage("/count", ",i", {i32(value)});
    }
    double levelValue() const { return live.snapshot()[0].value; }
    double countValue() const { return live.snapshot()[3].value; }
};
} // namespace

TEST_CASE("OSC-5a immediate past and due bundles apply at the right time",
          "[osc5a][osc]") {
    Osc5aFixture fixture;
    const auto now = ntp(1000);
    fixture.osc.receive(makeBundle(1, {fixture.level(0.25f)}), now);
    fixture.osc.receive(makeBundle(now - 1, {fixture.level(0.5f)}), now);
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 0);
    REQUIRE(fixture.osc.snapshotJson().at("applied") == 2);
    REQUIRE(fixture.osc.snapshotJson().at("recent")[0].at("scheduled") == false);

    const auto due = now + 10;
    fixture.osc.receive(makeBundle(due, {fixture.level(0.75f)}), now, "sender:1");
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 1);
    REQUIRE(fixture.osc.snapshotJson().at("recent").size() == 2);
    fixture.osc.releaseDue(due - 1);
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 1);
    REQUIRE(fixture.osc.snapshotJson().at("applied") == 2);
    fixture.osc.releaseDue(due);
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 0);
    REQUIRE(fixture.osc.snapshotJson().at("recent").size() == 3);
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("scheduled") == true);
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("sender") ==
            "sender:1");
    fixture.live.freeze(false, 2);
    REQUIRE(fixture.levelValue() == Catch::Approx(0.75));
}

TEST_CASE("OSC-5a one scheduled bundle is one atomic stage batch",
          "[osc5a][osc]") {
    Osc5aFixture fixture;
    const auto now = ntp(1000);
    const auto due = now + 100;
    fixture.osc.receive(
        makeBundle(due, {fixture.level(0.75f), fixture.count(7)}), now);
    const auto before = fixture.osc.snapshotJson()
                            .at("staged_batches").get<std::uint64_t>();
    REQUIRE(before == 0);
    fixture.osc.releaseDue(due);
    REQUIRE(fixture.osc.snapshotJson().at("staged_batches") == before + 1);
    REQUIRE(fixture.osc.snapshotJson().at("recent").size() == 2);

    std::atomic_bool release_again{true};
    std::thread writer([&] {
        std::uint64_t clock = due + 1;
        while (release_again.load(std::memory_order_acquire)) {
            fixture.osc.receive(
                makeBundle(clock, {fixture.level(0.75f), fixture.count(7)}),
                clock);
            ++clock;
        }
    });
    for (int frame = 2; frame < 1000; ++frame) {
        fixture.live.freeze(false, static_cast<std::uint64_t>(frame));
        const bool initial = fixture.levelValue() == Catch::Approx(0.0) &&
                             fixture.countValue() == Catch::Approx(0.0);
        const bool complete = fixture.levelValue() == Catch::Approx(0.75) &&
                              fixture.countValue() == Catch::Approx(7.0);
        REQUIRE((initial || complete));
    }
    release_again.store(false, std::memory_order_release);
    writer.join();
}

TEST_CASE("OSC-5a release orders time arrival and the NTP wrap independently",
          "[osc5a][osc]") {
    Osc5aFixture fixture;
    const auto now = ntp(1000);
    fixture.osc.receive(makeBundle(now + 20, {fixture.level(0.3f)}), now);
    fixture.osc.receive(makeBundle(now + 10, {fixture.level(0.2f)}), now);
    fixture.osc.releaseDue(now + 10);
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("value")
                .get<double>() == Catch::Approx(0.2));
    fixture.osc.releaseDue(now + 20);
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("value")
                .get<double>() == Catch::Approx(0.3));

    fixture.osc.receive(makeBundle(now + 30, {fixture.level(0.4f)}), now);
    fixture.osc.receive(makeBundle(now + 30, {fixture.level(0.5f)}), now);
    fixture.osc.releaseDue(now + 30);
    const auto recent = fixture.osc.snapshotJson().at("recent");
    REQUIRE(recent[recent.size() - 2].at("value").get<double>() ==
            Catch::Approx(0.4));
    REQUIRE(recent.back().at("value").get<double>() == Catch::Approx(0.5));

    Osc5aFixture wrapped;
    const auto before_wrap = ntp(0xfffffffeu);
    wrapped.osc.receive(
        makeBundle(ntp(0u, 0x40000000u), {wrapped.level(0.8f)}), before_wrap);
    wrapped.osc.receive(
        makeBundle(ntp(0xffffffffu, 0x80000000u), {wrapped.level(0.7f)}),
        before_wrap);
    wrapped.osc.releaseDue(ntp(0u, 0x80000000u));
    const auto wrapped_recent = wrapped.osc.snapshotJson().at("recent");
    REQUIRE(wrapped_recent.size() == 2);
    REQUIRE(wrapped_recent[0].at("value").get<double>() == Catch::Approx(0.7));
    REQUIRE(wrapped_recent[1].at("value").get<double>() == Catch::Approx(0.8));
}


TEST_CASE("OSC-5a scheduling window messages entries and bytes are bounded",
          "[osc5a][osc][limits]") {
    Osc5aFixture fixture;
    const auto now = ntp(1000);
    const auto future = makeBundle(now + (10ULL << 32) + 1,
                                   {fixture.level(0.5f)});
    fixture.osc.receive(future, now);
    REQUIRE(fixture.osc.snapshotJson().at("dropped").at("future_timetag") == 1);
    fixture.osc.receive(makeBundle(now + (10ULL << 32),
                                   {fixture.level(0.5f)}), now);
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 1);

    SECTION("empty bundles are consumed and never occupy entries") {
        Osc5aFixture empty;
        const auto packet = makeBundle(now + (1ULL << 32), {});
        for (int i = 0; i < 4096; ++i)
            empty.osc.receive(packet, now);
        REQUIRE(empty.osc.snapshotJson().at("received") == 4096);
        REQUIRE(empty.osc.snapshotJson().at("scheduled") == 0);
        REQUIRE(empty.osc.snapshotJson().at("dropped").at("scheduled_full") == 0);
    }

    SECTION("a bundle that exceeds the message room is dropped whole") {
        Osc5aFixture bounded;
        std::vector<Bytes> messages;
        for (int i = 0; i < 256; ++i) messages.push_back(bounded.level(0.5f));
        const auto packet = makeBundle(now + (1ULL << 32), messages);
        for (int i = 0; i < 4; ++i) bounded.osc.receive(packet, now);
        REQUIRE(bounded.osc.snapshotJson().at("scheduled") == 1024);
        bounded.osc.receive(makeBundle(now + (2ULL << 32),
                                       {bounded.level(0.5f), bounded.count(1)}),
                            now);
        REQUIRE(bounded.osc.snapshotJson().at("scheduled") == 1024);
        REQUIRE(bounded.osc.snapshotJson().at("dropped").at("scheduled_full") == 1);
        REQUIRE(bounded.osc.snapshotJson().at("recent").empty());
    }

    SECTION("the entry limit rejects the next nonempty bundle") {
        Osc5aFixture bounded;
        const auto packet = makeBundle(now + (1ULL << 32), {bounded.level(0.5f)});
        for (int i = 0; i < 1024; ++i) bounded.osc.receive(packet, now);
        REQUIRE(bounded.osc.snapshotJson().at("scheduled") == 1024);
        bounded.osc.receive(packet, now);
        REQUIRE(bounded.osc.snapshotJson().at("dropped").at("scheduled_full") == 1);
    }

    SECTION("longest messages reject only the bundle beyond the byte limit") {
        Osc5aFixture bounded;
        Bytes message;
        putString(message, "/x");
        putString(message, ",s");
        putString(message, std::string(65472, 'x'));
        const auto packet = makeBundle(now + (1ULL << 32), {message});
        REQUIRE(packet.size() == 65504);
        for (int i = 0; i < 256; ++i) bounded.osc.receive(packet, now);
        REQUIRE(bounded.osc.snapshotJson().at("scheduled") == 256);
        REQUIRE(bounded.osc.snapshotJson().at("dropped").at("scheduled_full") == 0);
        bounded.osc.receive(packet, now);
        REQUIRE(bounded.osc.snapshotJson().at("scheduled") == 256);
        REQUIRE(bounded.osc.snapshotJson().at("dropped").at("scheduled_full") == 1);
    }
}

TEST_CASE("OSC-5a replay refuses held and newly received future bundles",
          "[osc5a][osc][replay]") {
    Osc5aFixture fixture;
    const auto now = ntp(1000);
    const auto due = ntp(1001);
    fixture.osc.receive(makeBundle(due, {fixture.level(0.75f)}), now);
    fixture.live.freeze(true, 2);
    fixture.osc.releaseDue(due);
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 0);
    REQUIRE(fixture.osc.snapshotJson().at("dropped").at("replay") == 1);
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("reason") == "replay");
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("scheduled") == true);

    fixture.osc.receive(makeBundle(due, {fixture.level(0.5f)}), now);
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 0);
    REQUIRE(fixture.osc.snapshotJson().at("dropped").at("replay") == 2);
    fixture.live.freeze(false, 3);
    fixture.osc.releaseDue(due + 1);
    fixture.live.freeze(false, 4);
    REQUIRE(fixture.levelValue() == Catch::Approx(0.0));
}

TEST_CASE("OSC-5a starting and ending replay before due time discards the queue",
          "[osc5a][osc][replay]") {
    auto &live = GET_MODULE(LiveInputs);
    if (live.isLiveReplaying()) live.endLiveReplay();
    live.setDeclarationJson(liveDeclaration);
    live.freeze(false, 1);
    auto &osc = GET_MODULE(OscInput);
    osc.discardScheduled();
    osc.setBindings({{"/level", "level", LiveInputType::floating, false, {}}});
    const auto before = osc.snapshotJson().at("dropped").at("replay")
                            .get<std::uint64_t>();
    osc.receive(makeBundle(ntp(1001), {makeMessage("/level", ",f", {f32(0.75f)})}),
                ntp(1000));
    REQUIRE(osc.snapshotJson().at("scheduled") == 1);

    LiveInputRecordingRuntime runtime;
    auto recording = std::make_shared<LiveInputRecording>();
    recording->inputs = live.declarations();
    const LiveRecordingClock clock;
    runtime.startReplay(recording, clock);
    REQUIRE(osc.snapshotJson().at("scheduled") == 0);
    REQUIRE(osc.snapshotJson().at("dropped").at("replay") == before + 1);
    REQUIRE(osc.snapshotJson().at("recent").back().at("scheduled") == true);
    runtime.stopReplay();
    osc.releaseDue(ntp(1002));
    live.freeze(false, 2);
    REQUIRE(live.snapshot()[0].value == Catch::Approx(0.0));
    REQUIRE(osc.snapshotJson().at("dropped").at("replay") == before + 1);
}

TEST_CASE("OSC-5a release uses new bindings and stop discard is explicit",
          "[osc5a][osc]") {
    Osc5aFixture fixture;
    const auto now = ntp(1000);
    fixture.osc.receive(makeBundle(ntp(1001), {fixture.level(0.75f)}), now);
    fixture.osc.setBindings({});
    fixture.osc.releaseDue(ntp(1001));
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("reason") == "unbound");
    REQUIRE(fixture.osc.snapshotJson().at("recent").back().at("scheduled") == true);

    fixture.osc.setBindings(
        {{"/level", "level", LiveInputType::floating, false, {}}});
    fixture.osc.receive(makeBundle(ntp(1002), {fixture.level(0.5f)}), now);
    REQUIRE(fixture.osc.discardScheduled() == 1);
    REQUIRE(fixture.osc.snapshotJson().at("scheduled") == 0);
    fixture.osc.releaseDue(ntp(1003));
    fixture.live.freeze(false, 2);
    REQUIRE(fixture.levelValue() == Catch::Approx(0.0));
}
