#include "../src/core/renderer/viewfamily.hpp"
#include "../src/core/renderer/directionalshadowcascade.hpp"
#include "../src/core/renderer/planarreflectionview.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

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
    "render view family tokens are stable and domain-specific",
    "[view-family][token][gpu-abi]") {
    const auto main =
        renderViewFamilyToken(
            mainRenderViewFamilyId);
    REQUIRE(
        main ==
        renderViewFamilyToken(
            mainRenderViewFamilyId));
    REQUIRE(
        main !=
        renderViewFamilyToken(
            planarReflectionRenderViewFamilyId));
    REQUIRE_THROWS(
        renderViewFamilyToken(""));
}

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
    "render view family collection separates main cardinality from secondary families",
    "[view-family][collection][secondary]") {
    const auto xr_policy =
        compileGraphVariantPolicy({
            .variant =
                RenderPipelineGraphVariant::xr,
        });
    auto main = stereoFamily(
        view("$xr/0", 1.0f),
        view("$xr/1", 2.0f));
    RenderViewFamily shadow{
        .family_id =
            std::string{
                directionalShadowRenderViewFamilyId},
        .views = {
            view(
                std::string{
                    directionalShadowRenderViewId},
                3.0f),
        },
    };
    RenderViewFamilies families{
        .families = {main, shadow},
    };

    REQUIRE_NOTHROW(
        validateRenderViewFamilies(
            families, xr_policy));
    REQUIRE(
        families.require(
            mainRenderViewFamilyId)
            .views.size() == 2);
    REQUIRE(
        families.require(
            directionalShadowRenderViewFamilyId)
            .views.size() == 1);

    TemporalViewFamilyHistory shadow_history;
    REQUIRE(
        synchronizeTemporalViewFamilyHistory(
            shadow_history, shadow) ==
        TemporalViewFamilyChange::topology);
    const auto snapshots =
        buildRenderViewFamilySnapshots(
            shadow_history, shadow, {},
            xr_policy, 1, 2048, 2048,
            true);
    REQUIRE(snapshots.size() == 1);
    REQUIRE(
        snapshots.front()
            .projection_non_jittered[0][0] ==
        3.0f);

    families.families.push_back(shadow);
    REQUIRE_THROWS_WITH(
        validateRenderViewFamilies(
            families, xr_policy),
        "render view families contain duplicate family_id "
        "'$shadow/directional'");

    RenderViewFamilies missing_main{
        .families = {shadow},
    };
    REQUIRE_THROWS_WITH(
        validateRenderViewFamilies(
            missing_main, xr_policy),
        Catch::Matchers::ContainsSubstring(
            "do not provide family '$main'"));
}

TEST_CASE(
    "directional cascade provider creates stable camera-relative family views",
    "[view-family][shadow][cascade]") {
    RenderViewFamily main{
        .family_id =
            std::string{
                mainRenderViewFamilyId},
        .views = {
            RenderViewParameters{
                .view =
                    glm::lookAt(
                        glm::vec3{0.0f, 1.0f, 5.0f},
                        glm::vec3{0.0f, 1.0f, 0.0f},
                        glm::vec3{0.0f, 1.0f, 0.0f}),
                .projection =
                    glm::perspectiveRH_ZO(
                        glm::radians(70.0f),
                        16.0f / 9.0f,
                        0.1f, 200.0f),
                .camera_position =
                    {0.0f, 1.0f, 5.0f},
                .view_id = "camera",
            },
        },
    };
    const auto cascades =
        buildDirectionalShadowCascadeFamily(
            main,
            {-0.5f, -1.0f, -0.25f},
            {2048u, 2048u},
            DirectionalShadowCascadeSettings{
                .cascade_count = 3,
                .max_distance = 40.0f,
                .split_lambda = 0.7f,
                .stabilize = true,
            });

    REQUIRE(
        cascades.family_id ==
        "$shadow/directional");
    REQUIRE(cascades.views.size() == 3);
    float prior_far = 0.0f;
    for (std::uint32_t index = 0;
         index < cascades.views.size();
         ++index) {
        const auto &cascade =
            cascades.views[index];
        INFO("cascade " << index);
        REQUIRE(
            cascade.view_id ==
            "$cascade/" +
                std::to_string(index));
        REQUIRE(cascade.depth_range);
        REQUIRE(
            cascade.depth_range
                    ->near_distance <
            cascade.depth_range
                    ->far_distance);
        if (index != 0) {
            REQUIRE(
                cascade.depth_range
                    ->near_distance ==
                Catch::Approx(prior_far));
        }
        prior_far =
            cascade.depth_range
                ->far_distance;
        for (glm::length_t column = 0;
             column < 4; ++column) {
            for (glm::length_t row = 0;
                 row < 4; ++row) {
                REQUIRE(std::isfinite(
                    cascade
                        .view[column][row]));
                REQUIRE(std::isfinite(
                    cascade
                        .projection[column][row]));
            }
        }
    }
    REQUIRE(
        prior_far ==
        Catch::Approx(40.0f)
            .margin(0.01f));

    auto stereo = main;
    stereo.views.push_back(
        stereo.views.front());
    stereo.views[0].view_id = "left";
    stereo.views[1].view_id = "right";
    stereo.views[0].view =
        glm::translate(
            stereo.views[0].view,
            glm::vec3{0.03f, 0.0f, 0.0f});
    stereo.views[1].view =
        glm::translate(
            stereo.views[1].view,
            glm::vec3{-0.03f, 0.0f, 0.0f});
    REQUIRE(
        buildDirectionalShadowCascadeFamily(
            stereo,
            {-0.5f, -1.0f, -0.25f},
            {1024u, 1024u},
            DirectionalShadowCascadeSettings{
                .cascade_count = 3,
                .max_distance = 40.0f,
            })
            .views.size() == 3);
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
    "planar reflection views preserve identity, clipping, and raster winding",
    "[view-family][reflection][clip-plane]") {
    RenderViewFamily main{
        .family_id =
            std::string{
                mainRenderViewFamilyId},
        .views = {
            RenderViewParameters{
                .view =
                    glm::lookAt(
                        glm::vec3{1.0f, 2.0f, 3.0f},
                        glm::vec3{0.0f, 0.0f, 0.0f},
                        glm::vec3{0.0f, 1.0f, 0.0f}),
                .projection =
                    glm::perspectiveRH_ZO(
                        glm::radians(60.0f),
                        1.5f, 0.1f, 100.0f),
                .camera_position =
                    {1.0f, 2.0f, 3.0f},
                .view_id = "left",
            },
            RenderViewParameters{
                .view =
                    glm::lookAt(
                        glm::vec3{-1.0f, 4.0f, 5.0f},
                        glm::vec3{0.0f, 1.0f, 0.0f},
                        glm::vec3{0.0f, 1.0f, 0.0f}),
                .projection =
                    glm::perspectiveRH_ZO(
                        glm::radians(55.0f),
                        1.5f, 0.1f, 100.0f),
                .camera_position =
                    {-1.0f, 4.0f, 5.0f},
                .view_id = "right",
            },
        },
    };
    const auto reflection =
        buildPlanarReflectionViewFamily(
            main,
            PlanarReflectionViewSettings{
                .clip_plane =
                    RenderViewClipPlane{
                        .normal =
                            {0.0f, 2.0f, 0.0f},
                        .offset = -2.0f,
                    },
            });

    REQUIRE(
        reflection.family_id ==
        std::string{
            planarReflectionRenderViewFamilyId});
    REQUIRE(reflection.views.size() == 2);
    REQUIRE(
        reflection.views[0].view_id ==
        "$mirror/left");
    REQUIRE(
        reflection.views[1].view_id ==
        "$mirror/right");
    REQUIRE(
        reflection.views[0]
            .camera_position.x ==
        Catch::Approx(1.0f));
    REQUIRE(
        reflection.views[0]
            .camera_position.y ==
        Catch::Approx(0.0f));
    REQUIRE(
        reflection.views[0]
            .camera_position.z ==
        Catch::Approx(3.0f));
    REQUIRE(
        reflection.views[1]
            .camera_position.y ==
        Catch::Approx(-2.0f));
    REQUIRE(
        reflection.views[0]
            .projection[0][0] ==
        Catch::Approx(
            -main.views[0]
                 .projection[0][0]));
    glm::mat4 clip_x_flip{1.0f};
    clip_x_flip[0][0] = -1.0f;
    const auto unmodified_reflection_projection =
        clip_x_flip *
        main.views[0].projection;
    bool near_row_changed = false;
    for (glm::length_t column = 0;
         column < 4; ++column) {
        REQUIRE(
            reflection.views[0]
                .projection[column][0] ==
            Catch::Approx(
                unmodified_reflection_projection
                    [column][0]));
        REQUIRE(
            reflection.views[0]
                .projection[column][1] ==
            Catch::Approx(
                unmodified_reflection_projection
                    [column][1]));
        REQUIRE(
            reflection.views[0]
                .projection[column][3] ==
            Catch::Approx(
                unmodified_reflection_projection
                    [column][3]));
        near_row_changed =
            near_row_changed ||
            reflection.views[0]
                    .projection[column][2] !=
                Catch::Approx(
                    unmodified_reflection_projection
                        [column][2]);
    }
    REQUIRE(near_row_changed);
    const auto reflected_clip =
        [&](glm::vec3 world_position,
            const glm::mat4 &projection) {
            return projection *
                   reflection.views[0].view *
                   glm::vec4{
                       world_position, 1.0f};
        };
    const auto on_plane =
        reflected_clip(
            {0.0f, 1.0f, 0.0f},
            reflection.views[0].projection);
    const auto retained =
        reflected_clip(
            {0.0f, 1.5f, 0.0f},
            reflection.views[0].projection);
    const auto rejected =
        reflected_clip(
            {0.0f, 0.5f, 0.0f},
            reflection.views[0].projection);
    REQUIRE(
        on_plane.z ==
        Catch::Approx(0.0f)
            .margin(1.0e-5f));
    REQUIRE(on_plane.w > 0.0f);
    REQUIRE(retained.z > 0.0f);
    REQUIRE(retained.z < retained.w);
    REQUIRE(rejected.z < 0.0f);

    const auto jittered_projection =
        applyProjectionJitter(
            reflection.views[0].projection,
            {0.01f, -0.02f});
    REQUIRE(
        reflected_clip(
            {0.0f, 1.0f, 0.0f},
            jittered_projection)
            .z ==
        Catch::Approx(0.0f)
            .margin(1.0e-5f));
    REQUIRE(
        reflection.views[0].clip_plane);
    REQUIRE(
        reflection.views[0]
            .clip_plane->normal ==
        glm::vec3{0.0f, 1.0f, 0.0f});
    REQUIRE(
        reflection.views[0]
            .clip_plane->offset ==
        Catch::Approx(-1.0f));

    const auto policy =
        compileGraphVariantPolicy({
            .variant =
                RenderPipelineGraphVariant::xr,
        });
    RenderViewFamilies families{
        .families = {
            main,
            reflection,
        },
    };
    REQUIRE_NOTHROW(
        validateRenderViewFamilies(
            families, policy));

    REQUIRE_THROWS_AS(
        buildPlanarReflectionViewFamily(
            main,
            PlanarReflectionViewSettings{
                .clip_plane =
                    RenderViewClipPlane{
                        .normal =
                            {0.0f, 0.0f, 0.0f},
                    },
            }),
        std::invalid_argument);

    const auto without_oblique =
        buildPlanarReflectionViewFamily(
            main,
            PlanarReflectionViewSettings{
                .clip_plane =
                    RenderViewClipPlane{
                        .normal =
                            {0.0f, 1.0f, 0.0f},
                        .offset = -1.0f,
                    },
                .oblique_near_plane =
                    false,
            });
    for (glm::length_t column = 0;
         column < 4; ++column) {
        REQUIRE(
            without_oblique.views[0]
                .projection[column][2] ==
            Catch::Approx(
                unmodified_reflection_projection
                    [column][2]));
    }
}

TEST_CASE(
    "zero-to-one oblique projection supports orthographic cameras and safe fallback",
    "[view-family][reflection][oblique]") {
    const auto view_matrix =
        glm::lookAt(
            glm::vec3{0.0f, -2.0f, 0.0f},
            glm::vec3{0.0f, 1.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f});
    const auto projection =
        glm::orthoRH_ZO(
            -3.0f, 2.0f,
            -2.0f, 4.0f,
            0.1f, 20.0f);
    const RenderViewClipPlane plane{
        .normal =
            {0.0f, 1.0f, 0.0f},
        .offset = 0.0f,
    };
    const auto oblique =
        tryBuildObliqueNearPlaneProjectionZO(
            projection,
            view_matrix,
            plane);
    REQUIRE(oblique);
    const auto clip =
        [&](glm::vec3 world_position) {
            return *oblique *
                   view_matrix *
                   glm::vec4{
                       world_position, 1.0f};
        };
    REQUIRE(
        clip({0.0f, 0.0f, 0.0f}).z ==
        Catch::Approx(0.0f)
            .margin(1.0e-5f));
    REQUIRE(
        clip({0.0f, 1.0f, 0.0f}).z >
        0.0f);
    REQUIRE(
        clip({0.0f, -1.0f, 0.0f}).z <
        0.0f);

    const auto camera_in_retained_half_space =
        glm::lookAt(
            glm::vec3{0.0f, 2.0f, 0.0f},
            glm::vec3{0.0f, -1.0f, 0.0f},
            glm::vec3{0.0f, 0.0f, 1.0f});
    REQUIRE_FALSE(
        tryBuildObliqueNearPlaneProjectionZO(
            projection,
            camera_in_retained_half_space,
            plane));

    const RenderViewClipPlane receding_plane{
        .normal =
            {10.0f, -0.1f, 0.0f},
        .offset = -1.0f,
    };
    REQUIRE_FALSE(
        tryBuildObliqueNearPlaneProjectionZO(
            projection,
            view_matrix,
            receding_plane));

    REQUIRE_THROWS_AS(
        tryBuildObliqueNearPlaneProjectionZO(
            glm::mat4{0.0f},
            view_matrix,
            plane),
        std::invalid_argument);
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
