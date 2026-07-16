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
    model.material_primitives.push_back({material, {}});
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

} // namespace Pelican
