#include "../src/core/animation/animationservice.hpp"
#include "../src/core/animation/vrmapplication.hpp"
#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"
#include "fixtures/animation_abi/vrm_application_fixture_protocol.hpp"
#include "morph_fixture.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string_view>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace Pelican::Vrm {
namespace {

using Animation::Status;

VrmExpression expressionWithMorph(int node, int index, double weight) {
    VrmExpression expression;
    expression.morph_target_binds.push_back({node, index, weight});
    return expression;
}

struct Sandbox {
    std::filesystem::path root;
    ~Sandbox() { std::filesystem::remove_all(root); }
};

Sandbox makeSandbox() {
    auto root = std::filesystem::temp_directory_path() /
                ("pelican_wp123_" +
                 std::to_string(std::chrono::steady_clock::now()
                                    .time_since_epoch().count()));
    std::filesystem::create_directories(root);
    return {root};
}

void requireVulkan() {
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    try {
        (void)GET_MODULE(StandardMaterialResource);
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan unavailable: "} + error.what());
    }
}

#if defined(_WIN32)
struct FixtureDll {
    HMODULE module{};
    explicit FixtureDll(const char *path) : module(LoadLibraryA(path)) {
        REQUIRE(module != nullptr);
    }
    ~FixtureDll() {
        if (module) FreeLibrary(module);
    }
    template <class T> T symbol(const char *name) const {
        const auto address = GetProcAddress(module, name);
        REQUIRE(address != nullptr);
        return reinterpret_cast<T>(address);
    }
};
#endif

} // namespace

TEST_CASE("VRM expression resolution applies override, binary, lookAt and Base plus deltas",
          "[wp123][vrm][expression][unit]") {
    VrmSemanticData semantic;
    auto happy = expressionWithMorph(5, 0, 1.0);
    happy.material_color_binds = {
        {.material = 0, .type = "color", .target_value = {1.0, 0.2, 0.3, 1.0}},
        {.material = 0, .type = "emissionColor", .target_value = {0.5, 0.4, 0.3, 1.0}},
        {.material = 0, .type = "shadeColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
        {.material = 0, .type = "matcapColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
        {.material = 0, .type = "rimColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
        {.material = 0, .type = "outlineColor", .target_value = {1.0, 1.0, 1.0, 1.0}},
    };
    happy.texture_transform_binds = {
        {.material = 0, .scale = {2.0, 0.5}, .offset = {0.3, -0.1}},
    };
    happy.override_blink = "blend";
    semantic.preset_expressions.emplace("happy", happy);
    VrmExpression blink;
    blink.is_binary = true;
    semantic.preset_expressions.emplace("blink", blink);
    semantic.preset_expressions.emplace("lookLeft", VrmExpression{});
    semantic.preset_expressions.emplace("lookRight", VrmExpression{});
    semantic.preset_expressions.emplace("lookUp", VrmExpression{});
    semantic.preset_expressions.emplace("lookDown", VrmExpression{});
    semantic.look_at = VrmLookAtData{
        .type = "expression",
        .horizontal_outer = VrmLookAtRangeMap{45.0, 1.0},
        .vertical_down = VrmLookAtRangeMap{30.0, 0.8},
        .vertical_up = VrmLookAtRangeMap{0.0, 0.6},
    };

    MorphTargetLayout layout;
    layout.generation = 8;
    layout.default_weights = {0.9f};
    layout.primitives.push_back({.node_index = 5,
                                 .weight_offset = 0,
                                 .delta_ranges = {{.target_index = 0}}});
    SourceMaterialInitialValueTable initial;
    initial.values.push_back({.source_material_index = 0,
                              .base_color_factor = {0.2f, 0.4f, 0.6f, 1.0f},
                              .emissive_factor = {0.1f, 0.2f, 0.1f, 1.0f},
                              .uv_offset = {0.1f, 0.1f},
                              .uv_scale = {1.0f, 1.0f},
                              .uv_rotation = 0.25f});
    ExpressionInputSnapshot snapshot;
    snapshot.input_revision = 3;
    snapshot.frame_revision = 4;
    snapshot.look_at_enabled = true;
    snapshot.look_at_yaw_degrees = 22.5f;
    snapshot.look_at_pitch_degrees = -1.0f;
    snapshot.expression_weights = {{"happy", 0.5f}, {"blink", 1.0f}};
    REQUIRE(evaluateExpressionLookAt(semantic, snapshot) == Status::ok);
    REQUIRE(snapshot.expression_weights.at("lookLeft") ==
            Catch::Approx(0.5f));
    REQUIRE(snapshot.expression_weights.at("lookRight") == 0.0f);
    REQUIRE(snapshot.expression_weights.at("lookUp") ==
            Catch::Approx(0.6f));

    ResolvedExpressionFrame resolved;
    REQUIRE(resolveExpressionFrame(semantic, &layout, &initial, snapshot,
                                   resolved) == Status::ok);
    REQUIRE(resolved.expression_weights.at("blink") == 0.0f);
    REQUIRE(resolved.morph_weights == std::vector<float>{0.5f});
    REQUIRE(resolved.material_overrides.size() == 1);
    const auto &material = resolved.material_overrides.front();
    REQUIRE(material.base_color_factor.x == Catch::Approx(0.6f));
    REQUIRE(material.base_color_factor.y == Catch::Approx(0.3f));
    REQUIRE(material.emissive_factor.x == Catch::Approx(0.3f));
    REQUIRE(material.uv_offset.x == Catch::Approx(0.2f));
    REQUIRE(material.uv_scale.x == Catch::Approx(1.5f));
    REQUIRE(material.uv_rotation == Catch::Approx(0.25f));
    REQUIRE(resolved.diagnostics.size() == 4);
    REQUIRE(resolved.diagnostics[0].material_color_type == "shadeColor");
    REQUIRE(resolved.diagnostics[1].material_color_type == "matcapColor");
    REQUIRE(resolved.diagnostics[2].material_color_type == "rimColor");
    REQUIRE(resolved.diagnostics[3].material_color_type == "outlineColor");
}

#if defined(_WIN32)
TEST_CASE("third-party DLL publishes a VRM 1.0 expression frame through the additive public service",
          "[wp123][vrm][expression][dll][gpu]") {
    setupLogger();
    Animation::animationServiceRuntime().reset();
    applicationServiceRuntime().reset();
    auto sandbox = makeSandbox();
    const auto path = sandbox.root / "expression.vrm";
    TestMorphFixture::writeGlb(path, {.skinned = true,
                                     .vrm_expression = true});

    FastModuleContainer modules;
    requireVulkan();
    const auto model = GET_MODULE(GltfLoader).loadGltfBinary(path.string());
    REQUIRE(model.vrm_semantic);
    REQUIRE(model.morph_targets);
    REQUIRE(model.material_initial_values);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto polygon = instances.placeModelInstance(model);
    const auto instance = instances.animationInstance(polygon);

    ApplicationServiceV1 service{};
    service.struct_size = sizeof(service);
    service.version = applicationDescriptorVersionV1;
    REQUIRE(getApplicationServiceV1(applicationServiceVersionV1, &service) ==
            Status::ok);
    REQUIRE((service.capability_bits & applicationServiceCapabilitiesV1) ==
            applicationServiceCapabilitiesV1);

    using EvaluatorFn = std::uint32_t (*)(const ApplicationServiceV1 *,
                                           Animation::InstanceHandle,
                                           VrmApplicationFixtureResult *);
    FixtureDll evaluator{PELICAN_VRM_APPLICATION_EVALUATOR_DLL};
    VrmApplicationFixtureResult result{};
    REQUIRE(evaluator.symbol<EvaluatorFn>(
                "pelican_vrm_application_public_evaluator")(
                &service, instance, &result) ==
            static_cast<std::uint32_t>(Status::ok));
    REQUIRE(result.status == static_cast<std::uint32_t>(Status::ok));
    REQUIRE(result.happy_weight == Catch::Approx(0.75f));
    REQUIRE(result.morph_weight_count == 1);
    REQUIRE(result.material_override_count == 1);
    REQUIRE(result.diagnostic_count == 4);
    REQUIRE(result.diagnostic_code == static_cast<std::uint32_t>(
                                          ApplicationDiagnosticCodeV1::
                                              unsupported_material_color_type));
    REQUIRE(std::string{result.expression_name} == "happy");
    REQUIRE(std::string{result.material_color_type} == "shadeColor");

    const auto &morph = instances.morphWeightFrameForTesting(polygon);
    REQUIRE(morph.current_revision == 77);
    REQUIRE(morph.current == std::vector<float>{0.75f});
    const auto *material =
        instances.materialAbsoluteOverrideFrameForTesting(polygon, 0);
    REQUIRE(material != nullptr);
    REQUIRE(material->current_revision == 77);
    REQUIRE(material->current.base_color_factor.x == Catch::Approx(0.775f));
    REQUIRE(material->current.emissive_factor.x == Catch::Approx(0.305f));
    REQUIRE(material->current.uv_offset.x == Catch::Approx(0.15f));
    REQUIRE(material->current.uv_scale.x == Catch::Approx(1.375f));
    GET_MODULE(VulkanManageCore).waitIdle();
    applicationServiceRuntime().reset();
    Animation::animationServiceRuntime().reset();
}
#endif

} // namespace Pelican::Vrm
