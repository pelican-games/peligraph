#include "../src/core/container.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/standardmaterialresource.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/vkcore/core.hpp"

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>

namespace Pelican {
namespace {

void requireOverrideVulkan() {
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    try {
        (void)GET_MODULE(StandardMaterialResource);
    } catch (const std::exception &error) {
        SKIP(std::string{"Vulkan unavailable: "} + error.what());
    }
}

GlobalMaterialId registerTemplateMaterial() {
    const auto &standard = GET_MODULE(StandardMaterialResource);
    MaterialInfo info{
        .vert_shader = standard.standardVertShader(),
        .frag_shader = standard.standardFragShader(),
        .base_color_texture = standard.whiteTexture(),
        .metallic_roughness_texture = standard.metallicRoughnessDefaultTexture(),
        .normal_texture = standard.normalDefaultTexture(),
        .emissive_texture = standard.emissiveDefaultTexture(),
        .base_color_factor = {0.8f, 0.7f, 0.6f, 1.0f},
        .emissive_factor = {0.1f, 0.2f, 0.3f},
    };
    return GET_MODULE(MaterialContainer).registerMaterial(std::move(info));
}

ModelTemplate sharedTemplate(GlobalMaterialId material) {
    ModelTemplate model;
    model.asset_id = ModelAssetId{122};
    model.material_primitives.push_back(ModelTemplate::MaterialPrimitives{
        .material = material,
        .primitives = {},
        .source_material_index = 0,
    });
    auto initial = std::make_shared<SourceMaterialInitialValueTable>();
    initial->values.push_back(SourceMaterialInitialValues{
        .source_material_index = 0,
        .base_color_factor = {0.8f, 0.7f, 0.6f, 1.0f},
        .emissive_factor = {0.1f, 0.2f, 0.3f, 1.0f},
    });
    model.material_initial_values = std::move(initial);
    return model;
}

ModelTemplate twoMaterialTemplate(GlobalMaterialId first_material,
                                  GlobalMaterialId second_material) {
    auto model = sharedTemplate(first_material);
    model.asset_id = ModelAssetId{1222};
    model.material_primitives.push_back(ModelTemplate::MaterialPrimitives{
        .material = second_material,
        .primitives = {},
        .source_material_index = 1,
    });
    auto initial = std::make_shared<SourceMaterialInitialValueTable>();
    initial->values = {
        SourceMaterialInitialValues{
            .source_material_index = 0,
            .base_color_factor = {0.0f, 0.0f, 0.0f, 1.0f},
            .emissive_factor = {0.0f, 0.0f, 0.0f, 1.0f},
        },
        SourceMaterialInitialValues{
            .source_material_index = 1,
            .base_color_factor = {0.5f, 0.6f, 0.7f, 1.0f},
            .emissive_factor = {0.2f, 0.1f, 0.0f, 1.0f},
        },
    };
    model.material_initial_values = std::move(initial);
    return model;
}

PublishMaterialInstanceOverrideDescV1 overrideDesc(
    Animation::InstanceHandle instance, std::uint64_t revision,
    glm::vec4 base_color, glm::vec4 emissive = glm::vec4{1.0f},
    std::uint32_t mask = materialOverrideBaseColor) {
    PublishMaterialInstanceOverrideDescV1 result;
    result.instance = instance;
    result.frame_revision = revision;
    result.values.mask = mask;
    result.values.base_color_factor = base_color;
    result.values.emissive_factor = emissive;
    result.values.uv_offset = {0.125f, -0.25f};
    result.values.uv_scale = {2.0f, 0.5f};
    result.values.uv_rotation = 0.25f;
    return result;
}

PublishMaterialInstanceAbsoluteOverrideDescV2 absoluteOverrideDesc(
    Animation::InstanceHandle instance, std::uint64_t revision,
    std::uint32_t source_material_index, glm::vec4 base_color,
    glm::vec4 emissive = glm::vec4{0.0f},
    std::uint32_t mask = materialOverrideBaseColor) {
    PublishMaterialInstanceAbsoluteOverrideDescV2 result;
    result.instance = instance;
    result.frame_revision = revision;
    result.source_material_index = source_material_index;
    result.values.mask = mask;
    result.values.base_color_factor = base_color;
    result.values.emissive_factor = emissive;
    result.values.uv_offset = {0.125f, -0.25f};
    result.values.uv_scale = {2.0f, 0.5f};
    result.values.uv_rotation = 0.25f;
    return result;
}

bool sameBytes(const std::vector<std::byte> &left,
               const std::vector<std::byte> &right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

} // namespace

TEST_CASE("material instance override frames are independent and preserve templates",
          "[wp122][material-instance][temporal][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireOverrideVulkan();

    auto &materials = GET_MODULE(MaterialContainer);
    const auto material = registerTemplateMaterial();
    const auto template_before = materials.materialGpuRecordForTesting(material);
    const auto descriptor_revision = materials.materialDescriptorRevisionForTesting(material);
    auto model = sharedTemplate(material);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto first = instances.placeModelInstance(model);
    const auto second = instances.placeModelInstance(model);

    REQUIRE(instances.materialOverrideStorageEntryCountForTesting() == 0);
    const auto first_handle = instances.animationInstance(first);
    const auto second_handle = instances.animationInstance(second);
    auto red = overrideDesc(first_handle, 7, {1.0f, 0.15f, 0.1f, 1.0f},
                            {0.4f, 0.1f, 0.1f, 1.0f}, materialOverrideAll);
    auto blue = overrideDesc(second_handle, 7, {0.1f, 0.25f, 1.0f, 1.0f});
    REQUIRE(instances.publishMaterialInstanceOverride(first, red) ==
            Animation::Status::ok);
    REQUIRE(instances.publishMaterialInstanceOverride(second, blue) ==
            Animation::Status::ok);

    const auto *first_frame = instances.materialOverrideFrameForTesting(first);
    const auto *second_frame = instances.materialOverrideFrameForTesting(second);
    REQUIRE(first_frame != nullptr);
    REQUIRE(second_frame != nullptr);
    REQUIRE(first_frame->current_revision == 7);
    REQUIRE(second_frame->current_revision == 7);
    REQUIRE(first_frame->current.base_color_factor ==
            glm::vec4{1.0f, 0.15f, 0.1f, 1.0f});
    REQUIRE(second_frame->current.base_color_factor ==
            glm::vec4{0.1f, 0.25f, 1.0f, 1.0f});
    REQUIRE(first_frame->previous.base_color_factor ==
            first_frame->current.base_color_factor);
    REQUIRE(model.material_primitives.front().material == material);
    REQUIRE(sameBytes(materials.materialGpuRecordForTesting(material), template_before));
    REQUIRE(materials.materialDescriptorRevisionForTesting(material) == descriptor_revision);

    instances.advanceMaterialOverrideHistoryAfterRender();
    auto green = overrideDesc(first_handle, 8, {0.1f, 1.0f, 0.2f, 1.0f});
    REQUIRE(instances.publishMaterialInstanceOverride(first, green) ==
            Animation::Status::ok);
    first_frame = instances.materialOverrideFrameForTesting(first);
    REQUIRE(first_frame->current_revision == 8);
    REQUIRE(first_frame->previous_revision == 7);
    REQUIRE(first_frame->previous.base_color_factor ==
            glm::vec4{1.0f, 0.15f, 0.1f, 1.0f});

    instances.resetTemporalHistory();
    first_frame = instances.materialOverrideFrameForTesting(first);
    REQUIRE(first_frame->previous_revision == first_frame->current_revision);
    REQUIRE(first_frame->previous.base_color_factor ==
            first_frame->current.base_color_factor);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("material instance override validates atomically and reuses storage",
          "[wp122][material-instance][validation][stress][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireOverrideVulkan();

    auto &materials = GET_MODULE(MaterialContainer);
    const auto material = registerTemplateMaterial();
    const auto template_before = materials.materialGpuRecordForTesting(material);
    auto model = sharedTemplate(material);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.placeModelInstance(model);
    const auto handle = instances.animationInstance(instance);

    for (std::uint64_t revision = 1; revision <= 1000; ++revision) {
        const float phase = (revision & 1u) == 0u ? 0.25f : 0.75f;
        auto frame = overrideDesc(handle, revision, {phase, 1.0f - phase, 0.5f, 1.0f},
                                  {1.0f, phase, 1.0f - phase, 1.0f},
                                  materialOverrideAll);
        REQUIRE(instances.publishMaterialInstanceOverride(instance, frame) ==
                Animation::Status::ok);
        instances.advanceMaterialOverrideHistoryAfterRender();
    }
    REQUIRE(instances.materialOverrideStorageEntryCountForTesting() == 1);
    REQUIRE(sameBytes(materials.materialGpuRecordForTesting(material), template_before));

    auto invalid = overrideDesc(handle, 1001, glm::vec4{1.0f});
    invalid.values.uv_rotation = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(instances.publishMaterialInstanceOverride(instance, invalid) ==
            Animation::Status::invalid_argument);
    const auto *unchanged = instances.materialOverrideFrameForTesting(instance);
    REQUIRE(unchanged != nullptr);
    REQUIRE(unchanged->current_revision == 1000);

    invalid.values.uv_rotation = 0.0f;
    invalid.values.mask = materialOverrideAll | (1u << 31u);
    REQUIRE(instances.publishMaterialInstanceOverride(instance, invalid) ==
            Animation::Status::invalid_argument);
    REQUIRE(instances.materialOverrideFrameForTesting(instance)->current_revision == 1000);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("model reload resets material instance override generation",
          "[wp122][material-instance][reload][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireOverrideVulkan();

    const auto material = registerTemplateMaterial();
    auto model = sharedTemplate(material);
    auto replacement = sharedTemplate(material);
    replacement.content_revision = 2;
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.placeModelInstance(model);
    const auto old_handle = instances.animationInstance(instance);
    auto frame = overrideDesc(old_handle, 1, {1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(instances.publishMaterialInstanceOverride(instance, frame) ==
            Animation::Status::ok);

    instances.rebuildModelInstances(model.asset_id, replacement);
    REQUIRE(instances.materialOverrideFrameForTesting(instance) == nullptr);
    REQUIRE(instances.materialOverrideStorageEntryCountForTesting() == 0);
    REQUIRE(instances.animationInstance(instance).identity == old_handle.identity);
    REQUIRE(instances.animationInstance(instance).generation != old_handle.generation);
    frame.frame_revision = 2;
    REQUIRE(instances.publishMaterialInstanceOverride(instance, frame) ==
            Animation::Status::stale_generation);
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("material absolute overrides are source-material scoped and temporal",
          "[wp122b][material-instance][absolute][temporal][stress][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireOverrideVulkan();

    auto &materials = GET_MODULE(MaterialContainer);
    const auto first_material = registerTemplateMaterial();
    const auto second_material = registerTemplateMaterial();
    const auto first_bytes =
        materials.materialGpuRecordForTesting(first_material);
    const auto second_bytes =
        materials.materialGpuRecordForTesting(second_material);
    auto model = twoMaterialTemplate(first_material, second_material);
    REQUIRE(model.material_initial_values->values.at(0).base_color_factor ==
            glm::vec4{0.0f, 0.0f, 0.0f, 1.0f});

    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto first = instances.placeModelInstance(model);
    const auto second = instances.placeModelInstance(model);
    const auto first_handle = instances.animationInstance(first);
    const auto second_handle = instances.animationInstance(second);

    auto red = absoluteOverrideDesc(first_handle, 1, 0,
                                    {1.0f, 0.0f, 0.0f, 1.0f},
                                    {0.3f, 0.0f, 0.0f, 1.0f},
                                    materialOverrideAll);
    auto blue = absoluteOverrideDesc(second_handle, 1, 0,
                                     {0.0f, 0.0f, 1.0f, 1.0f});
    auto green = absoluteOverrideDesc(first_handle, 1, 1,
                                      {0.0f, 1.0f, 0.0f, 1.0f});
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(first, red) ==
            Animation::Status::ok);
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(second, blue) ==
            Animation::Status::ok);
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(first, green) ==
            Animation::Status::ok);
    REQUIRE(instances.materialAbsoluteOverrideStorageEntryCountForTesting() == 3);
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(first, 0)
                ->current.base_color_factor ==
            glm::vec4{1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(first, 0)
                ->current.emissive_factor ==
            glm::vec4{0.3f, 0.0f, 0.0f, 1.0f});
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(first, 0)
                ->current.uv_offset == glm::vec2{0.125f, -0.25f});
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(first, 1)
                ->current.base_color_factor ==
            glm::vec4{0.0f, 1.0f, 0.0f, 1.0f});
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(second, 0)
                ->current.base_color_factor ==
            glm::vec4{0.0f, 0.0f, 1.0f, 1.0f});

    instances.advanceMaterialOverrideHistoryAfterRender();
    red.frame_revision = 2;
    red.values.base_color_factor = {1.0f, 0.5f, 0.0f, 1.0f};
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(first, red) ==
            Animation::Status::ok);
    const auto *first_frame =
        instances.materialAbsoluteOverrideFrameForTesting(first, 0);
    REQUIRE(first_frame->previous_revision == 1);
    REQUIRE(first_frame->previous.base_color_factor ==
            glm::vec4{1.0f, 0.0f, 0.0f, 1.0f});
    instances.resetTemporalHistory();
    first_frame = instances.materialAbsoluteOverrideFrameForTesting(first, 0);
    REQUIRE(first_frame->previous_revision == first_frame->current_revision);
    REQUIRE(first_frame->previous.base_color_factor ==
            first_frame->current.base_color_factor);

    for (std::uint64_t revision = 3; revision <= 1002; ++revision) {
        red.frame_revision = revision;
        red.values.base_color_factor.r =
            (revision & 1u) == 0u ? 0.25f : 0.75f;
        REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(first, red) ==
                Animation::Status::ok);
        instances.advanceMaterialOverrideHistoryAfterRender();
    }
    REQUIRE(instances.materialAbsoluteOverrideStorageEntryCountForTesting() == 3);

    auto invalid = red;
    invalid.frame_revision = 1003;
    invalid.source_material_index = 2;
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(first, invalid) ==
            Animation::Status::invalid_argument);
    invalid.source_material_index = 0;
    invalid.values.base_color_factor.x =
        std::numeric_limits<float>::quiet_NaN();
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(first, invalid) ==
            Animation::Status::invalid_argument);
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(first, 0)
                ->current_revision == 1002);
    REQUIRE(sameBytes(materials.materialGpuRecordForTesting(first_material),
                      first_bytes));
    REQUIRE(sameBytes(materials.materialGpuRecordForTesting(second_material),
                      second_bytes));
    GET_MODULE(VulkanManageCore).waitIdle();
}

TEST_CASE("model reload resets material absolute override generation",
          "[wp122b][material-instance][absolute][reload][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    requireOverrideVulkan();

    const auto first_material = registerTemplateMaterial();
    const auto second_material = registerTemplateMaterial();
    auto model = twoMaterialTemplate(first_material, second_material);
    auto replacement = twoMaterialTemplate(first_material, second_material);
    replacement.content_revision = 2;
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto instance = instances.placeModelInstance(model);
    const auto old_handle = instances.animationInstance(instance);
    auto frame = absoluteOverrideDesc(old_handle, 1, 0,
                                      {1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(instance, frame) ==
            Animation::Status::ok);

    instances.rebuildModelInstances(model.asset_id, replacement);
    REQUIRE(instances.materialAbsoluteOverrideFrameForTesting(instance, 0) ==
            nullptr);
    REQUIRE(instances.materialAbsoluteOverrideStorageEntryCountForTesting() == 0);
    REQUIRE(instances.animationInstance(instance).identity == old_handle.identity);
    REQUIRE(instances.animationInstance(instance).generation != old_handle.generation);
    frame.frame_revision = 2;
    REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(instance, frame) ==
            Animation::Status::stale_generation);
    GET_MODULE(VulkanManageCore).waitIdle();
}

} // namespace Pelican
