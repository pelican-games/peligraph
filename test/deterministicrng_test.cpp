#include "../src/core/container.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <nlohmann/json.hpp>

namespace Pelican {

namespace {

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

nlohmann::json projectWithSeed(std::uint64_t seed) {
    return nlohmann::json{
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", "deterministic-rng"},
        {"engine_min_version", "0.1.0"},
        {"basic_config", {{"seed", seed}}},
    };
}

} // namespace

TEST_CASE("GameContext random sequence matches fixed PCG32 fixture", "[determinism]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ProjectSource).setProjectData(projectWithSeed(42).dump());

    GameContext ctx;
    REQUIRE(ctx.seed() == 42);
    REQUIRE(ctx.random() == Catch::Approx(0.44214480546293677).epsilon(1e-15));
    REQUIRE(ctx.randomInt(-10, 10) == -2);
    REQUIRE(ctx.randomFloat(-1.0f, 1.0f) == Catch::Approx(-0.70481666527304387f).epsilon(1e-6));
    REQUIRE(ctx.randomInt(0, 1000000) == 545615);
}

TEST_CASE("GameContext setSeed reproduces the same random stream", "[determinism]") {
    ensureLogger();
    FastModuleContainer modules;

    GameContext ctx;
    ctx.setSeed(777);
    const auto first_random = ctx.random();
    const auto first_int = ctx.randomInt(-50, 50);
    const auto first_float = ctx.randomFloat(10.0f, 20.0f);
    const auto first_negative_range = ctx.randomInt(-3, 2);

    ctx.setSeed(777);
    REQUIRE(ctx.seed() == 777);
    REQUIRE(ctx.random() == Catch::Approx(first_random).epsilon(1e-15));
    REQUIRE(ctx.randomInt(-50, 50) == first_int);
    REQUIRE(ctx.randomFloat(10.0f, 20.0f) == Catch::Approx(first_float).epsilon(1e-6));
    REQUIRE(ctx.randomInt(-3, 2) == first_negative_range);

    REQUIRE(first_random == Catch::Approx(0.80658582233829246).epsilon(1e-15));
    REQUIRE(first_int == -38);
    REQUIRE(first_float == Catch::Approx(12.107573152313321f).epsilon(1e-6));
    REQUIRE(first_negative_range == -3);
}

TEST_CASE("GameContext randomInt handles inclusive boundaries", "[determinism]") {
    ensureLogger();
    FastModuleContainer modules;

    GameContext ctx;
    ctx.setSeed(0);
    REQUIRE(ctx.randomInt(5, 5) == 5);
    REQUIRE(ctx.randomInt(-3, 2) == -2);
    REQUIRE(ctx.randomInt(-3, 2) == 1);
    REQUIRE(ctx.randomInt(-3, 2) == 1);
    REQUIRE_THROWS(ctx.randomInt(2, -3));
}

} // namespace Pelican
