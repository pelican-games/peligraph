#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../src/core/openxr/openxrviewspace.hpp"
#include "../src/core/vkcore/renderer.hpp"

#include <array>
#include <cmath>
#include <string>

namespace {

using Catch::Approx;

XrView identityView() {
    XrView view{XR_TYPE_VIEW};
    view.pose.orientation.w = 1.0F;
    view.fov.angleLeft = -std::atan(1.0F);
    view.fov.angleRight = std::atan(1.0F);
    view.fov.angleDown = -std::atan(1.0F);
    view.fov.angleUp = std::atan(1.0F);
    return view;
}

void requireMatrix(const glm::mat4 &actual, const glm::mat4 &expected,
                   float margin = 0.00001F) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            REQUIRE(actual[column][row] == Approx(expected[column][row]).margin(margin));
        }
    }
}

glm::vec3 ndc(const glm::mat4 &projection, const glm::vec3 &view_position) {
    const auto clip = projection * glm::vec4{view_position, 1.0F};
    return glm::vec3{clip} / clip.w;
}

} // namespace

TEST_CASE("OpenXR identity pose produces identity world/view and zero camera position",
          "[openxr][view-space][identity]") {
    const auto result = Pelican::OpenXr::buildRenderViewParameters(
        glm::mat4{1.0F}, identityView(), 0.1F, 100.0F);

    requireMatrix(result.view, glm::mat4{1.0F});
    CHECK(result.camera_position.x == Approx(0.0F));
    CHECK(result.camera_position.y == Approx(0.0F));
    CHECK(result.camera_position.z == Approx(0.0F));
    CHECK(result.first_person_view);
}

TEST_CASE("OpenXR 64mm IPD keeps eye positions and inverse view translations distinct",
          "[openxr][view-space][ipd]") {
    std::array views{identityView(), identityView()};
    views[0].pose.position.x = -0.032F;
    views[1].pose.position.x = 0.032F;

    const auto result = Pelican::OpenXr::buildRenderViewParameters(
        glm::mat4{1.0F}, views, 0.1F, 100.0F);
    REQUIRE(result.size() == 2);
    CHECK(result[0].first_person_view);
    CHECK(result[1].first_person_view);
    CHECK(result[0].camera_position.x == Approx(-0.032F));
    CHECK(result[1].camera_position.x == Approx(0.032F));
    CHECK(result[0].view[3][0] == Approx(0.032F));
    CHECK(result[1].view[3][0] == Approx(-0.032F));
    CHECK(result[0].view_id == "$xr/0");
    CHECK(result[1].view_id == "$xr/1");
    CHECK(result[1].camera_position.x - result[0].camera_position.x ==
          Approx(0.064F));

    const auto family = Pelican::OpenXr::buildMainRenderViewFamily(
        glm::mat4{1.0F}, views, 0.1F, 100.0F);
    CHECK(
        family.family_id ==
        std::string{Pelican::mainRenderViewFamilyId});
    REQUIRE(family.views.size() == 2);
    CHECK(family.views[0].view_id == "$xr/0");
    CHECK(family.views[1].view_id == "$xr/1");
}

TEST_CASE("flat render view defaults to third-person visibility",
          "[wp134][view-space][firstperson]") {
    const Pelican::RenderViewParameters flat;
    CHECK_FALSE(flat.first_person_view);
}

TEST_CASE("OpenXR positive 90 degree yaw composes after active world-from-stage",
          "[openxr][view-space][yaw]") {
    auto view = identityView();
    constexpr float sqrt_half = 0.7071067811865475F;
    view.pose.orientation.y = sqrt_half;
    view.pose.orientation.w = sqrt_half;

    glm::mat4 active_camera_view{1.0F};
    active_camera_view[3] = {-1.0F, -2.0F, -3.0F, 1.0F};
    const auto result = Pelican::OpenXr::buildRenderViewParameters(
        active_camera_view, view, 0.1F, 100.0F);

    const glm::mat4 expected_view{
        glm::vec4{0.0F, 0.0F, 1.0F, 0.0F},
        glm::vec4{0.0F, 1.0F, 0.0F, 0.0F},
        glm::vec4{-1.0F, 0.0F, 0.0F, 0.0F},
        glm::vec4{3.0F, -2.0F, -1.0F, 1.0F},
    };
    requireMatrix(result.view, expected_view);
    CHECK(result.camera_position.x == Approx(1.0F));
    CHECK(result.camera_position.y == Approx(2.0F));
    CHECK(result.camera_position.z == Approx(3.0F));
}

TEST_CASE("OpenXR asymmetric RH ZO projection fixes frustum offsets depth and viewport Y",
          "[openxr][view-space][projection]") {
    XrFovf fov{
        .angleLeft = std::atan(-0.5F),
        .angleRight = std::atan(1.0F),
        .angleUp = std::atan(0.75F),
        .angleDown = std::atan(-0.25F),
    };
    const auto projection =
        Pelican::OpenXr::asymmetricProjectionRhZo(fov, 0.1F, 100.0F);

    glm::mat4 expected{0.0F};
    expected[0][0] = 1.3333333333F;
    expected[1][1] = -2.0F;
    expected[2][0] = 0.3333333333F;
    expected[2][1] = -0.5F;
    expected[2][2] = -1.0010010010F;
    expected[2][3] = -1.0F;
    expected[3][2] = -0.1001001001F;
    requireMatrix(projection, expected);

    CHECK(ndc(projection, {-0.05F, 0.0F, -0.1F}).x == Approx(-1.0F));
    CHECK(ndc(projection, {0.1F, 0.0F, -0.1F}).x == Approx(1.0F));
    CHECK(ndc(projection, {0.0F, 0.075F, -0.1F}).y == Approx(-1.0F));
    CHECK(ndc(projection, {0.0F, -0.025F, -0.1F}).y == Approx(1.0F));
    CHECK(ndc(projection, {0.0F, 0.0F, -0.1F}).z == Approx(0.0F).margin(0.00001F));
    CHECK(ndc(projection, {0.0F, 0.0F, -100.0F}).z == Approx(1.0F));
}

TEST_CASE("OpenXR reference spaces prefer floor-aware origins and expose LOCAL fallback",
          "[openxr][view-space][reference-space]") {
    constexpr std::array all_spaces{
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
        XR_REFERENCE_SPACE_TYPE_STAGE,
    };
    CHECK(Pelican::OpenXr::selectReferenceSpace(all_spaces) ==
          XR_REFERENCE_SPACE_TYPE_STAGE);

    constexpr std::array floor_and_local{
        XR_REFERENCE_SPACE_TYPE_LOCAL,
        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR,
    };
    CHECK(Pelican::OpenXr::selectReferenceSpace(floor_and_local) ==
          XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR);

    constexpr std::array local_only{XR_REFERENCE_SPACE_TYPE_LOCAL};
    const auto selected = Pelican::OpenXr::selectReferenceSpace(local_only);
    REQUIRE(selected == XR_REFERENCE_SPACE_TYPE_LOCAL);
    const auto status = Pelican::OpenXr::describeReferenceSpace(selected);
    CHECK(std::string{status.reference_space} == "LOCAL");
    CHECK(std::string{status.floor_semantics} == "floor_not_guaranteed");
    CHECK_FALSE(status.floor_level_guaranteed);
    CHECK(status.applied_floor_offset_m == Approx(0.0F));
}
