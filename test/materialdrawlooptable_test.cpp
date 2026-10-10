#include "../src/core/material/materialdrawlooptable.hpp"
#include "../src/core/material/materialregistration.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

namespace Pelican {
namespace {

// CPU storage stands in for GPU records; publication and step resolution use
// the same transaction and resolver as MaterialContainer/MaterialDrawLoopTable.
struct MaterialFamilyFixture {
    const GlobalMaterialId base{10};
    const GlobalMaterialId outline{11};
    bool deferred = false;
    bool allocated = false;
    bool fail_attach = false;
    bool declare_outline = true;
    unsigned publications = 0;
    unsigned rollbacks = 0;
    std::map<std::string, GlobalMaterialId> variants;
    std::array<std::vector<ResolvedMaterialDrawStep>, 2> resolved;

    void publish(GlobalMaterialId id) {
        REQUIRE(id == base);
        ++publications;
        for (const auto mode : vertexModes) {
            MaterialDrawSubject subject;
            subject.name = "Paint";
            subject.material_key = "paint.material.json#Paint";
            subject.skinned = mode == VertexMode::skinned;
            subject.key = materialDrawSubjectKey(
                subject.name, "paint.material.json", "", std::nullopt,
                subject.skinned);
            subject.route = MaterialRouteClass::forward_opaque;
            subject.contract = MaterialShaderContract::forward_scene_color_v1;
            for (const auto &[name, resource] : variants) {
                (void)resource;
                subject.variants.push_back(name);
            }
            GraphDrawLoop loop;
            loop.loop = "sk3_outline";
            loop.pass = "forward_opaque";
            loop.contract = MaterialPassContract::forward_opaque_v1;
            GraphLoopDrawCondition condition;
            condition.material = subject.material_key;
            condition.skinned = subject.skinned;
            loop.branches.push_back({condition, MaterialDrawStepList{
                MaterialDrawStep::material(), MaterialDrawStep::named("outline")}});
            const auto rule = makeGraphDrawLoopPassRule(std::move(loop));
            const auto steps = rule.rule(subject);
            REQUIRE(steps.has_value());
            resolved.at(static_cast<std::size_t>(mode)) = resolveMaterialDrawSteps(
                base, subject, *steps, rule.node, false,
                [&](std::string_view name) { return variants.at(std::string{name}); },
                [](const MaterialDrawStep &) -> PipelineHandle {
                    FAIL("a named variant must not allocate an override pipeline");
                    return PipelineHandle{};
                });
        }
    }

    GlobalMaterialId registerFamily() {
        return registerMaterialFamily(deferred,
            [&] {
                allocated = true;
                if (!deferred) publish(base);
                return base;
            },
            [&](GlobalMaterialId id) {
                REQUIRE(id == base);
                if (fail_attach) throw std::runtime_error("variant allocation failed");
                // Hidden records nest their deferral inside the family scope.
                {
                    ScopedMaterialPublicationDeferral hidden{deferred};
                    if (declare_outline) variants.emplace("outline", outline);
                }
                if (!deferred) publish(base);
            },
            [&](GlobalMaterialId id) { publish(id); },
            [&](GlobalMaterialId id) {
                REQUIRE(id == base);
                ++rollbacks;
                allocated = false;
                variants.clear();
                for (auto &steps : resolved) steps.clear();
            });
    }
};

} // namespace

TEST_CASE("SK3 materialdrawlooptable resolves outline in both modes after family publication",
          "[material][draw_loop][sk3]") {
    MaterialFamilyFixture fixture;
    REQUIRE(fixture.registerFamily() == fixture.base);
    CHECK_FALSE(fixture.deferred);
    CHECK(fixture.publications == 1);
    CHECK(fixture.rollbacks == 0);
    const std::vector<ResolvedMaterialDrawStep> expected{
        {fixture.base, std::nullopt}, {fixture.outline, std::nullopt}};
    CHECK(fixture.resolved[0] == expected);
    CHECK(fixture.resolved[1] == expected);
}

TEST_CASE("SK3 materialdrawlooptable refuses only an undeclared variant and rolls back",
          "[material][draw_loop][sk3]") {
    MaterialFamilyFixture fixture;
    fixture.declare_outline = false;
    CHECK_THROWS_WITH(fixture.registerFamily(), Catch::Matchers::ContainsSubstring(
        "graph.loop_variant_missing@1"));
    CHECK(fixture.publications == 1);
    CHECK(fixture.rollbacks == 1);
    CHECK_FALSE(fixture.deferred);
    CHECK_FALSE(fixture.allocated);
    CHECK(fixture.variants.empty());
    CHECK(fixture.resolved[0].empty());
    CHECK(fixture.resolved[1].empty());
}

TEST_CASE("SK3 material family failed attachment does not publish and restores deferral",
          "[material][draw_loop][sk3]") {
    MaterialFamilyFixture fixture;
    fixture.fail_attach = true;
    CHECK_THROWS_WITH(fixture.registerFamily(), "variant allocation failed");
    CHECK(fixture.publications == 0);
    CHECK(fixture.rollbacks == 1);
    CHECK_FALSE(fixture.allocated);
    CHECK_FALSE(fixture.deferred);

    fixture.fail_attach = false;
    fixture.deferred = true;
    REQUIRE(fixture.registerFamily() == fixture.base);
    CHECK(fixture.deferred);
    CHECK(fixture.publications == 0);
    CHECK(fixture.variants.at("outline") == fixture.outline);
}

} // namespace Pelican
