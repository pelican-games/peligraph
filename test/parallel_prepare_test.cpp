#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "../src/core/parallel_prepare.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace Pelican {

TEST_CASE("parallel prepare retains declaration order across repeated runs", "[startup][models]") {
    const std::vector<std::string> declarations{"slow-first", "fast-second", "medium-third", "fast-fourth"};
    const auto run = [&] {
        const auto prepared = parallelPrepareOrdered<std::string>(declarations.size(), [&](std::size_t index) {
            const auto delay = std::chrono::milliseconds{static_cast<int>((declarations.size() - index) * 3)};
            std::this_thread::sleep_for(delay);
            return declarations[index] + "-prepared";
        });

        std::vector<std::string> registered;
        for (const auto &item : prepared) {
            registered.push_back(item);
        }
        return registered;
    };

    const auto expected = std::vector<std::string>{"slow-first-prepared", "fast-second-prepared",
                                                   "medium-third-prepared", "fast-fourth-prepared"};
    REQUIRE(run() == expected);
    REQUIRE(run() == expected);
}

TEST_CASE("parallel prepare propagates worker failures only after joining", "[startup][models]") {
    REQUIRE_THROWS_WITH(parallelPrepareOrdered<int>(4, [](std::size_t index) {
                            if (index == 2) {
                                throw std::runtime_error("prepare failed");
                            }
                            return static_cast<int>(index);
                        }),
                        "prepare failed");
}

} // namespace Pelican
