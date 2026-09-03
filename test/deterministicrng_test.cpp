#include "../src/core/container.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/sceneloadfault.hpp"
#include "../src/core/loader/sceneparticipantmanifest.hpp"
#include "../src/core/log.hpp"
#include "../src/core/userpublic/deterministicrng.hpp"
#include "../src/core/userpublic/gamecontext.hpp"
#include "scene_load_fault_test_support.hpp"

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

TEST_CASE("WP380 RNG sequence overlay commits or rolls back as one queue column",
          "[WP380][transaction][rng]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ProjectSource).setProjectData(projectWithSeed(380).dump());

    auto &rng = GET_MODULE(DeterministicRng);
    GameContext ctx;
    const auto before = rng.prepareSequence();

    auto discarded = rng.prepareSequence();
    {
        RngSequenceScope scope{discarded};
        ctx.setSeed(381);
        (void)ctx.random();
        (void)ctx.randomInt(-10, 10);
        REQUIRE(ctx.seed() == 381);
    }
    const auto after_discard = rng.prepareSequence();
    REQUIRE(after_discard.current_seed == before.current_seed);
    REQUIRE(after_discard.state == before.state);
    REQUIRE(after_discard.increment == before.increment);
    REQUIRE(discarded.current_seed == 381);
    REQUIRE(discarded.state != before.state);

    auto committed = rng.prepareSequence();
    {
        RngSequenceScope scope{committed};
        ctx.setSeed(382);
        (void)ctx.random();
        (void)ctx.randomInt(-10, 10);
        REQUIRE(ctx.seed() == 382);
    }
    rng.publishPreparedSequence(committed);
    const auto after_commit = rng.prepareSequence();
    REQUIRE(after_commit.current_seed == committed.current_seed);
    REQUIRE(after_commit.state == committed.state);
    REQUIRE(after_commit.increment == committed.increment);
}

TEST_CASE("WP380 RNG rollback bypass mutations leak only their manifest field",
          "[WP380][transaction][rng][mutation]") {
    ensureLogger();
    FastModuleContainer modules;
    GET_MODULE(ProjectSource).setProjectData(projectWithSeed(380).dump());
    auto &rng = GET_MODULE(DeterministicRng);

    struct FieldCase {
        std::string_view field;
        std::uint64_t DeterministicRng::PreparedSequence::*member;
    };
    constexpr FieldCase fields[]{
        {"current_seed",
         &DeterministicRng::PreparedSequence::current_seed},
        {"state", &DeterministicRng::PreparedSequence::state},
    };
    const DeterministicRng::PreparedSequence before{11, 22, 33};
    const DeterministicRng::PreparedSequence candidate{111, 222, 333};

    for (const auto &field : fields) {
        const auto mutation = test_support::manifestMutationId(
            "rng_sequence", field.field);
        REQUIRE_FALSE(mutation.empty());
        INFO("mutation=" << mutation);
        test_support::ManifestSceneLoadFault injector{mutation};
        for (const bool enabled : {false, true}) {
            rng.publishPreparedSequence(before);
            injector.resetCalls();
            injector.setEnabled(enabled);
            internal::rollbackSceneRngCandidate(
                rng, candidate, &injector);
            const auto actual = rng.prepareSequence();
            REQUIRE(injector.saw(SceneLoadFaultPhase::flush,
                                 "rng_sequence"));
            REQUIRE(actual.*(field.member) ==
                    (enabled ? candidate.*(field.member)
                             : before.*(field.member)));
            for (const auto &other : fields) {
                if (other.member == field.member) continue;
                REQUIRE(actual.*(other.member) == before.*(other.member));
            }
            REQUIRE(actual.increment == before.increment);
        }
    }
}

} // namespace Pelican
