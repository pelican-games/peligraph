#include "../src/core/container.hpp"
#include "../src/core/os/liveinputs.hpp"
#include "../src/core/osc/oscinput.hpp"
#include "../src/core/osc/oscparser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace Pelican;

namespace {
using Bytes = std::vector<std::uint8_t>;

constexpr std::string_view declarations = R"({
  "schema":"pelican.live_inputs", "version":1,
  "inputs":[
    {"name":"level","type":"float","default":0.0,"min":-10.0,"max":10.0},
    {"name":"count","type":"int","default":0,"min":-100,"max":100},
    {"name":"enabled","type":"bool","default":false}
  ]
})";

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
void putI32(Bytes &out, std::int32_t value) {
    putU32(out, std::bit_cast<std::uint32_t>(value));
}
void putString(Bytes &out, std::string_view value) {
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(0);
    while ((out.size() & 3u) != 0) out.push_back(0);
}
Bytes message(std::string_view address, std::string_view tags) {
    Bytes out;
    putString(out, address);
    putString(out, tags);
    return out;
}
Bytes bundle(const Bytes &element, std::uint32_t claimed_size = 0) {
    Bytes out{'#','b','u','n','d','l','e',0};
    putU64(out, 0);
    putU32(out, claimed_size == 0 ? static_cast<std::uint32_t>(element.size())
                                 : claimed_size);
    out.insert(out.end(), element.begin(), element.end());
    return out;
}
std::string hex(const Bytes &bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (const auto byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
        out.push_back(' ');
    }
    return out;
}
std::uint64_t seedFromEnv() {
    if (const char *value = std::getenv("OSC_FUZZ_SEED0"))
        return std::strtoull(value, nullptr, 0);
    return 0x05c0de;
}
std::size_t countFromEnv(bool long_run) {
    if (const char *value = std::getenv("OSC_FUZZ_COUNT"))
        return static_cast<std::size_t>(std::strtoull(value, nullptr, 0));
    return long_run ? 2000000 : 20000;
}

struct Fuzz {
    std::mt19937_64 random;
    OscInput input;
    std::uint64_t received = 0;
    std::uint32_t tag = 0;
    std::array<std::uint64_t, 4> counters{};

    explicit Fuzz(std::uint64_t seed) : random(seed) {
        auto &live = GET_MODULE(LiveInputs);
        live.setDeclarationJson(declarations);
        live.freeze(false, 0);
        input.setBindings({
            {"/level", "level", LiveInputType::floating, false, {}},
            {"/count", "count", LiveInputType::integer, false, {}},
            {"/enabled", "enabled", LiveInputType::boolean, false, {}},
            {"/equal", "level", LiveInputType::floating, true,
             {{1.0, 1.0}, {-10.0, 10.0}}},
            {"/reverse", "level", LiveInputType::floating, true,
             {{1.0, -1.0}, {-10.0, 10.0}}},
        });
    }

    Bytes randomBytes(std::size_t size) {
        Bytes out(size);
        for (auto &byte : out) byte = static_cast<std::uint8_t>(random());
        return out;
    }
    Bytes validMessage() {
        Bytes out;
        switch (random() % 12) {
        case 0: case 1: case 2:
            out = message("/level", ",f");
            putU32(out, static_cast<std::uint32_t>(random())); break;
        case 3:
            out = message("/count", ",i"); putI32(out, std::numeric_limits<std::int32_t>::min()); break;
        case 4:
            out = message("/reverse", ",d"); putU64(out, random()); break;
        case 5:
            out = message("/equal", ",f"); putU32(out, 0x3f800000); break;
        case 6: {
            out = message("/level", ",s");
            const auto text = randomBytes(random() % 24);
            out.insert(out.end(), text.begin(), text.end()); break;
        }
        case 7:
            out = message("/level", ",b");
            putI32(out, static_cast<std::int32_t>(random())); break;
        case 8:
            out = message("/level", "," + std::string(1 + random() % 64, static_cast<char>(tag++)));
            break;
        case 9:
            out = message("/level", ",h"); putU64(out, random()); break;
        case 10:
            out = message("/enabled", ",T"); break;
        default:
            out = message("/fuzz" + std::to_string(random() % 8), ",r");
            putU32(out, static_cast<std::uint32_t>(random())); break;
        }
        if ((random() & 7u) == 0) {
            static constexpr std::array<double, 5> special{
                std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::denorm_min(), -0.0,
                std::numeric_limits<double>::max()};
            out = message("/level", ",f");
            putU32(out, std::bit_cast<std::uint32_t>(
                            static_cast<float>(special[random() % special.size()])));
        }
        return out;
    }

    Bytes next(std::size_t iteration) {
        switch (iteration % 10) {
        case 0: return randomBytes(4 * (random() % 65));
        case 1: {
            auto out = validMessage();
            if (!out.empty()) out[random() % out.size()] ^= std::uint8_t{1} << (random() % 8);
            return out;
        }
        case 2: {
            auto out = validMessage();
            out.resize(random() % (out.size() + 1));
            while ((out.size() & 3u) != 0 && !out.empty()) out.pop_back();
            return out;
        }
        case 3: {
            auto out = validMessage();
            const auto extra = randomBytes(4 * (1 + random() % 8));
            out.insert(out.end(), extra.begin(), extra.end());
            return out;
        }
        case 4: {
            auto out = validMessage();
            if (!out.empty()) out[(random() % (out.size() / 4 + 1)) * 4 % out.size()] = 0xff;
            return out;
        }
        case 5: {
            Bytes nested = validMessage();
            const auto depth = random() % 14;
            for (std::size_t i = 0; i < depth; ++i) nested = bundle(nested);
            return nested;
        }
        case 6: {
            auto out = validMessage();
            static constexpr std::array<std::uint32_t, 5> sizes{
                0, 4, 0x7ffffffc, 0x80000000, 0xffffffff};
            return bundle(out, sizes[random() % sizes.size()]);
        }
        case 7: {
            auto out = validMessage();
            return bundle(out, static_cast<std::uint32_t>(out.size() + 4 * (1 + random() % 100)));
        }
        case 8: {
            auto out = message("/level", ",f");
            putU32(out, static_cast<std::uint32_t>(random()));
            return out;
        }
        default: {
            Bytes nested = message("/level", ",f");
            putU32(nested, 0x3f800000);
            for (int i = 0; i < 3; ++i) nested = bundle(nested);
            auto out = bundle(nested);
            if (!out.empty()) out[random() % out.size()] ^= static_cast<std::uint8_t>(random());
            return out;
        }
        }
    }
};

void runFuzz(std::size_t count, std::uint64_t seed) {
    Fuzz fuzz{seed};
    std::uint64_t now = 0xffffffffULL << 32;
    for (std::size_t i = 0; i < count; ++i) {
        auto bytes = fuzz.next(i);
        now += static_cast<std::uint32_t>(fuzz.random());
        if ((i & 1u) != 0) {
            std::uint64_t timetag = 0;
            switch (fuzz.random() % 6) {
            case 0: timetag = 1; break;
            case 1: timetag = now - (fuzz.random() % (4ULL << 32)); break;
            case 2: timetag = now + (fuzz.random() % (10ULL << 32)); break;
            case 3: timetag = now + (10ULL << 32) + 1 +
                              (fuzz.random() % (10ULL << 32)); break;
            case 4: timetag = now + (fuzz.random() & 0xffffffffu); break;
            default: timetag = fuzz.random(); break;
            }
            bytes = bundle(bytes);
            for (int byte = 0; byte < 8; ++byte) {
                bytes[8 + byte] = static_cast<std::uint8_t>(
                    timetag >> (56 - byte * 8));
            }
        }
        const auto start = std::chrono::steady_clock::now();
        const auto packet = Osc::parsePacket(bytes);
        fuzz.input.receive(bytes, now, "fuzz");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        fuzz.input.releaseDue(now + fuzz.random() % (12ULL << 32));
        if (elapsed > std::chrono::milliseconds(50)) {
            INFO("input " << i << ": " << hex(bytes));
            FAIL("OSC processing exceeded 50 ms");
        }
        if (packet.drop == Osc::PacketDrop::none) {
            for (const auto &msg : packet.messages) {
                for (const unsigned char c : msg.address) {
                    if (c == 0 || c >= 0x80) {
                        INFO("input " << i << ": " << hex(bytes));
                        FAIL("accepted address is not NUL-free printable UTF-8");
                    }
                }
            }
        }
        auto state = fuzz.input.snapshotJson();
        const std::array<std::uint64_t, 4> current{
            state.at("received").get<std::uint64_t>(),
            state.at("applied").get<std::uint64_t>(),
            state.at("dropped").at("malformed").get<std::uint64_t>(),
            state.at("dropped").at("limit").get<std::uint64_t>()};
        if (!std::equal(fuzz.counters.begin(), fuzz.counters.end(), current.begin(),
                        [](auto before, auto after) { return after >= before; }) ||
            state.at("recent").size() > 64 ||
            state.at("scheduled").get<std::uint64_t>() > 1024) {
            INFO("input " << i << ": " << hex(bytes));
            FAIL("counter decreased or recent ring exceeded capacity");
        }
        fuzz.counters = current;
        auto &live = GET_MODULE(LiveInputs);
        live.freeze(false, i + 1);
        for (const auto &value : live.snapshot()) {
            if (!std::isfinite(value.value) ||
                (value.spec.minimum && value.value < *value.spec.minimum) ||
                (value.spec.maximum && value.value > *value.spec.maximum)) {
                INFO("input " << i << ": " << hex(bytes));
                FAIL("stored live value violates its declaration");
            }
        }
    }
    fuzz.input.releaseDue(now + (20ULL << 32));
    if (fuzz.input.snapshotJson().at("scheduled") != 0)
        FAIL("scheduled queue did not empty after time passed");
}
} // namespace

TEST_CASE("OSC parser and receive randomized short sanitizer fuzz", "[osc-fuzz-short]") {
    runFuzz(countFromEnv(false), seedFromEnv());
}
TEST_CASE("OSC parser and receive randomized long sanitizer fuzz", "[osc-fuzz-long][.]") {
    runFuzz(countFromEnv(true), seedFromEnv());
}

TEST_CASE("OSC receive and binding swaps are thread safe", "[tsan1]") {
    auto &live = GET_MODULE(LiveInputs);
    live.setDeclarationJson(declarations);
    OscInput input;
    Osc::OscBinding binding;
    binding.address = "/level";
    binding.input = "level";
    std::vector<Osc::OscBinding> bindings{binding};
    input.setBindings(bindings);
    auto packet = message("/level", ",f");
    putU32(packet, std::bit_cast<std::uint32_t>(1.0f));

    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            input.receive(packet, 0);
            if ((i % 10) == 0) input.setBindings(bindings);
        }
    });
    for (int i = 0; i < 1000; ++i) {
        live.freeze(false, static_cast<std::uint64_t>(i));
        const auto frozen = live.frozenInputs();
        CHECK(frozen.find("live:level") != frozen.end());
        CHECK(input.snapshotJson().at("recent").size() <= 64);
    }
    writer.join();
}

TEST_CASE("OSC fuzz regression: empty input", "[osc-fuzz]") {
    const Bytes input{};
    const auto packet = Osc::parsePacket(input);
    REQUIRE(packet.drop == Osc::PacketDrop::malformed);
}
