#include "animation/beziercurve.hpp"
#include "output/outputfile.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <string>

namespace {

using namespace Pelican::Animation;

std::uint64_t seed0() {
    if (const char *text = std::getenv("READ_FUZZ_SEED0"))
        return std::strtoull(text, nullptr, 0);
    return 0x9e3779b97f4a7c15ULL;
}

unsigned fuzzCount(unsigned fallback) {
    if (const char *text = std::getenv("READ_FUZZ_COUNT"))
        return static_cast<unsigned>(std::strtoul(text, nullptr, 0));
    return fallback;
}

std::string hex(double value) {
    std::ostringstream out;
    out << std::hexfloat << value;
    return out.str();
}

void finiteEvaluation(double at, double av, double odt, double odv,
                      double bt, double bv, double idt, double idv, double t) {
    const double value = bezierSegmentValue(at, av, odt, odv, bt, bv, idt, idv, t);
    INFO("at=" << hex(at) << " av=" << hex(av) << " out=[" << hex(odt) << ","
               << hex(odv) << "] bt=" << hex(bt) << " bv=" << hex(bv)
               << " in=[" << hex(idt) << "," << hex(idv) << "] t=" << hex(t)
               << " value=" << hex(value));
    CHECK(std::isfinite(value));
}

void bezierProperties(unsigned count, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<int> exponent(-300, 300);
    auto magnitude = [&] { return std::ldexp(unit(random), exponent(random)); };
    for (unsigned iteration = 0; iteration < count; ++iteration) {
        const double at = iteration % 13 == 0 ? -1.0e308 : magnitude() - 0.5;
        const double width = iteration % 17 == 0
                                 ? std::numeric_limits<double>::denorm_min()
                                 : std::max(std::numeric_limits<double>::denorm_min(), magnitude());
        const double bt = iteration % 13 == 0 ? 1.0e308 : at + width;
        if (!(bt > at)) continue;
        const double av = iteration % 19 == 0 ? 1.0e308 : magnitude() - 0.5;
        const double bv = iteration % 23 == 0 ? -1.0e308 : magnitude() - 0.5;
        const double full_width = bt - at;
        const double odt = unit(random) * full_width;
        const double idt = -unit(random) * full_width;
        const double odv = (unit(random) - 0.5) * magnitude();
        const double idv = (unit(random) - 0.5) * magnitude();
        const double s = unit(random);
        const double time = std::lerp(at, bt, s);
        finiteEvaluation(at, av, odt, odv, bt, bv, idt, idv, time);
        finiteEvaluation(at, av, odt, odv, bt, bv, idt, idv, at);
        finiteEvaluation(at, av, odt, odv, bt, bv, idt, idv, bt);

        double previous = at;
        for (unsigned sample = 0; sample <= 16; ++sample) {
            const auto point = bezierSegmentPointAtU(
                at, av, odt, odv, bt, bv, idt, idv, sample / 16.0);
            INFO("non-monotonic point at u=" << sample / 16.0);
            CHECK(point[0] >= previous);
            CHECK(std::isfinite(point[0]));
            CHECK(std::isfinite(point[1]));
            previous = point[0];
        }
        if (s > 0.0 && s < 1.0 && std::isfinite(full_width)) {
            const auto split = splitBezierSegment(at, av, odt, odv, bt, bv, idt, idv, time);
            if (split && count <= 1000) {
                for (unsigned sample = 0; sample <= 1000; ++sample) {
                    const double t = std::lerp(at, bt, sample / 1000.0);
                    const double expected = bezierSegmentValue(
                        at, av, odt, odv, bt, bv, idt, idv, t);
                    const double actual = t <= time
                        ? bezierSegmentValue(at, av, split->a_out_handle[0],
                                             split->a_out_handle[1], time, split->value,
                                             split->middle_in_handle[0], split->middle_in_handle[1], t)
                        : bezierSegmentValue(time, split->value, split->middle_out_handle[0],
                                             split->middle_out_handle[1], bt, bv,
                                             split->b_in_handle[0], split->b_in_handle[1], t);
                    const double tolerance = 2.0e-8 * std::max({1.0, std::abs(expected), std::abs(actual)});
                    INFO("split seed=" << seed << " iteration=" << iteration
                         << " at=" << hex(at) << " av=" << hex(av) << " out=[" << hex(odt) << ","
                         << hex(odv) << "] time=" << hex(time) << " bt=" << hex(bt)
                         << " bv=" << hex(bv) << " in=[" << hex(idt) << "," << hex(idv)
                         << "] t=" << hex(t) << " expected=" << hex(expected)
                         << " actual=" << hex(actual));
                    CHECK(std::abs(actual - expected) <= tolerance);
                }
            }
        }
    }
}

} // namespace

TEST_CASE("Bezier solver keeps an ordinary root bracket", "[read-fuzz-short]") {
    const auto u = bezier_detail::solveNormalised(
        0x1.12a593389c778p-1, 0x1.d96df98f4ad68p-2, 0x1.8p-1);
    REQUIRE(u);
    CHECK(bezier_detail::cubic(0.0, 0x1.12a593389c778p-1,
          0x1.d96df98f4ad68p-2, 1.0, *u) == 0x1.8p-1);
}

TEST_CASE("Bezier extreme overshoot split regression", "[read-fuzz-short][!mayfail]") {
    const double at = -0x1p-1, av = -0x1p-1;
    const double odt = 0x1.ba9e5f90c731bp-6, odv = 0x1.6ae2ac9b6c28cp-215;
    const double bt = -0x1.e2b5a52cc4f1ep-2, bv = 0x1.3eab257df8536p+155;
    const double idt = -0x1.c95b02d0de047p-6, idv = 0x1.3955bc54d0f81p+177;
    const double time = -0x1.e9b603981d1d8p-2, t = -0x1.ed8c095715acep-2;
    const auto split = splitBezierSegment(at, av, odt, odv, bt, bv, idt, idv, time);
    REQUIRE(split);
    const double expected = bezierSegmentValue(at, av, odt, odv, bt, bv, idt, idv, t);
    const double actual = bezierSegmentValue(at, av, split->a_out_handle[0],
        split->a_out_handle[1], time, split->value, split->middle_in_handle[0],
        split->middle_in_handle[1], t);
    CHECK(actual == Catch::Approx(expected).epsilon(1.0e-12));
}

TEST_CASE("Bezier fuzz properties", "[read-fuzz-short][!mayfail]") {
    bezierProperties(fuzzCount(100), seed0());
}

TEST_CASE("Bezier long fuzz properties", "[read-fuzz-long][.]") {
    bezierProperties(fuzzCount(1000000), seed0());
}

TEST_CASE("PNG exact budget and accumulation", "[read-fuzz-short]") {
    std::mt19937_64 random(seed0() ^ 0x504e47);
    const auto directory = std::filesystem::temp_directory_path() /
        ("pelican_read_fuzz_png_" + std::to_string(std::bit_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(directory);
    std::uint64_t remaining = 1000000;
    for (unsigned iteration = 0; iteration < fuzzCount(100); ++iteration) {
        Pelican::OutputFileImage image;
        image.width = 1 + random() % 16;
        image.height = 1 + random() % 16;
        image.pixels.resize(image.width * image.height * 4);
        std::generate(image.pixels.begin(), image.pixels.end(), [&] { return random() & 255; });
        const auto exact = Pelican::writeOutputFrameFile(
            directory, "exact", iteration, image, Pelican::OutputFileEncoding::png);
        REQUIRE(exact.status == Pelican::OutputFileWriteStatus::written);
        CHECK(Pelican::writeOutputFrameFile(directory, "small", iteration, image,
              Pelican::OutputFileEncoding::png, exact.bytes_written - 1).status ==
              Pelican::OutputFileWriteStatus::limit_reached);
        CHECK_FALSE(std::filesystem::exists(directory / ("small_" + std::string(6 - std::to_string(iteration).size(), '0') + std::to_string(iteration) + ".png.partial")));
        if (remaining >= exact.bytes_written) {
            const auto result = Pelican::writeOutputFrameFile(directory, "sum", iteration, image,
                Pelican::OutputFileEncoding::png, remaining);
            REQUIRE(result.status == Pelican::OutputFileWriteStatus::written);
            CHECK(result.bytes_written <= remaining);
            remaining -= result.bytes_written;
        }
    }
    std::filesystem::remove_all(directory);
}
