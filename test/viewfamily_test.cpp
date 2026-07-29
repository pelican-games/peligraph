#include "../src/core/renderer/viewfamily.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace Pelican {
namespace {

RenderViewParameters view(
    std::string id, float projection_x,
    float camera_x = 0.0f) {
    RenderViewParameters result;
    result.projection[0][0] = projection_x;
    result.camera_position.x = camera_x;
    result.view_id = std::move(id);
    return result;
}

RenderViewFamily stereoFamily(
    RenderViewParameters first,
    RenderViewParameters second) {
    return RenderViewFamily{
        .family_id =
            std::string{mainRenderViewFamilyId},
        .views = {
            std::move(first),
            std::move(second),
        },
    };
}

} // namespace

TEST_CASE(
    "render view family validates stable identities and graph cardinality",
    "[view-family][contract]") {
    const auto xr_policy =
        compileGraphVariantPolicy({
            .variant =
                RenderPipelineGraphVariant::xr,
        });
    auto family = stereoFamily(
        view("$xr/0", 1.0f),
        view("$xr/1", 2.0f));
    REQUIRE_NOTHROW(
        validateRenderViewFamily(
            family, xr_policy));

    family.views[1].view_id = "$xr/0";
    REQUIRE_THROWS_WITH(
        validateRenderViewFamily(
            family, xr_policy),
        "render view family '$main' contains duplicate view_id '$xr/0'");

    family.views.resize(1);
    family.views[0].view_id.clear();
    REQUIRE_THROWS_WITH(
        validateRenderViewFamily(
            family, xr_policy),
        "render view family '$main' contains a view without a stable view_id");

    family.views[0].view_id = "$xr/0";
    REQUIRE_THROWS_WITH(
        validateRenderViewFamily(
            family, xr_policy),
        "render graph variant 'xr' requires 2 views in family '$main'");
}

TEST_CASE(
    "temporal view family follows stable ids across execution reordering",
    "[view-family][history]") {
    const auto policy =
        compileGraphVariantPolicy({
            .variant =
                RenderPipelineGraphVariant::flat,
        });
    auto first = stereoFamily(
        view("left", 1.0f, -1.0f),
        view("right", 2.0f, 1.0f));
    TemporalViewFamilyHistory history;
    REQUIRE(
        synchronizeTemporalViewFamilyHistory(
            history, first) ==
        TemporalViewFamilyChange::topology);

    const RenderViewFamilyProjectionModifiers
        modifiers;
    auto first_snapshots =
        buildRenderViewFamilySnapshots(
            history, first, modifiers, policy,
            1, 200, 100, true);
    commitRenderViewFamilySnapshots(
        history, first, first_snapshots);

    auto reordered = stereoFamily(
        view("right", 4.0f, 2.0f),
        view("left", 3.0f, -2.0f));
    REQUIRE(
        synchronizeTemporalViewFamilyHistory(
            history, reordered) ==
        TemporalViewFamilyChange::
            execution_order);
    const auto reordered_snapshots =
        buildRenderViewFamilySnapshots(
            history, reordered, modifiers, policy,
            2, 200, 100, false);

    REQUIRE(
        reordered_snapshots[0]
            .previous_projection_non_jittered[0][0] ==
        2.0f);
    REQUIRE(
        reordered_snapshots[0]
            .previous_camera_position.x ==
        1.0f);
    REQUIRE(
        reordered_snapshots[1]
            .previous_projection_non_jittered[0][0] ==
        1.0f);
    REQUIRE(
        reordered_snapshots[1]
            .previous_camera_position.x ==
        -1.0f);
}

TEST_CASE(
    "view family projection jitter is one shared modifier sample",
    "[view-family][projection-modifier]") {
    const auto policy =
        compileGraphVariantPolicy({
            .variant =
                RenderPipelineGraphVariant::flat,
        });
    const auto family = stereoFamily(
        view("left", 1.0f),
        view("right", 1.0f));
    TemporalViewFamilyHistory history;
    REQUIRE(
        synchronizeTemporalViewFamilyHistory(
            history, family) ==
        TemporalViewFamilyChange::topology);

    RenderViewFamilyProjectionModifiers modifiers{
        .projection_jitter =
            ProjectionJitterSettings{
                .provider = "taa",
                .pattern =
                    ProjectionJitterPattern::halton23,
                .phases = 8,
            },
    };
    const auto snapshots =
        buildRenderViewFamilySnapshots(
            history, family, modifiers, policy,
            1, 200, 100, true);

    REQUIRE(snapshots.size() == 2);
    REQUIRE(
        snapshots[0].jitter_ndc ==
        snapshots[1].jitter_ndc);
    REQUIRE(
        snapshots[0].jitter_ndc.x ==
        Catch::Approx(0.0f));
    REQUIRE(
        snapshots[0].jitter_ndc.y ==
        Catch::Approx(-1.0f / 300.0f));

    const auto xr_policy =
        compileGraphVariantPolicy({
            .variant =
                RenderPipelineGraphVariant::xr,
        });
    REQUIRE_THROWS_WITH(
        buildRenderViewFamilySnapshots(
            history, family, modifiers,
            xr_policy, 1, 200, 100, true),
        "render graph variant 'xr' forbids projection jitter for family '$main'");
}

TEST_CASE(
    "view family membership changes invalidate the whole family topology",
    "[view-family][history][topology]") {
    auto family = stereoFamily(
        view("left", 1.0f),
        view("right", 1.0f));
    TemporalViewFamilyHistory history;
    REQUIRE(
        synchronizeTemporalViewFamilyHistory(
            history, family) ==
        TemporalViewFamilyChange::topology);
    history.views.at("left").valid = true;

    family.views[1].view_id = "observer";
    REQUIRE(
        synchronizeTemporalViewFamilyHistory(
            history, family) ==
        TemporalViewFamilyChange::topology);
    REQUIRE_FALSE(history.hasValidView());
    REQUIRE(history.views.contains("left"));
    REQUIRE(history.views.contains("observer"));
    REQUIRE_FALSE(history.views.contains("right"));
}

} // namespace Pelican
