#include "../src/core/vkcore/devicefeaturepolicy.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shaderreflection.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <string>

namespace Pelican {

TEST_CASE("device_feature_policy selects discrete Vulkan 1.3 devices and honors overrides", "[vulkan][device_feature_policy]") {
    std::vector<VulkanDeviceCandidate> devices{
        {"Integrated", VK_API_VERSION_1_3, vk::PhysicalDeviceType::eIntegratedGpu, true, true},
        {"Discrete", VK_API_VERSION_1_3, vk::PhysicalDeviceType::eDiscreteGpu, true, false},
        {"Old discrete", VK_API_VERSION_1_2, vk::PhysicalDeviceType::eDiscreteGpu, true, true},
    };
    REQUIRE(selectVulkanPhysicalDevice(devices) == 1);
    REQUIRE(selectVulkanPhysicalDevice(devices, "0") == 0);
    REQUIRE(selectVulkanPhysicalDevice(devices, "Integrated") == 0);
    REQUIRE_THROWS_WITH(selectVulkanPhysicalDevice(devices, "2"), Catch::Matchers::ContainsSubstring("Old discrete"));
    REQUIRE_THROWS(selectVulkanPhysicalDevice(devices, "missing"));
    devices[1].suitable = false;
    REQUIRE(selectVulkanPhysicalDevice(devices) == 0);
    devices[0].api_version = VK_API_VERSION_1_2;
    REQUIRE_THROWS_WITH(selectVulkanPhysicalDevice(devices), Catch::Matchers::ContainsSubstring("Vulkan 1.3"));
    REQUIRE_THROWS_WITH(selectVulkanPhysicalDevice(devices), Catch::Matchers::ContainsSubstring("Integrated"));
}

namespace {

bool hasExtension(const RayQueryDeviceSelection &selection,
                  const char *name) {
    return std::find(selection.device_extensions.begin(),
                     selection.device_extensions.end(), name) !=
           selection.device_extensions.end();
}

} // namespace

TEST_CASE("ray query bootstrap stays optional and enables its dependency set atomically",
          "[vulkan][ray-query][bootstrap]") {
    RayQueryDeviceSupport unavailable;
    RayQueryDeviceSelection disabled;
    REQUIRE_NOTHROW(
        disabled = selectRayQueryDeviceFeatures(unavailable));
    REQUIRE_FALSE(disabled.acceleration_structure);
    REQUIRE_FALSE(disabled.ray_query);
    REQUIRE_FALSE(disabled.buffer_device_address);
    REQUIRE_FALSE(disabled.ray_tracing_pipeline);
    REQUIRE_FALSE(disabled.ray_tracing_indirect);
    REQUIRE(disabled.ray_tracing_facts == RayTracingRuntimeFacts{});
    REQUIRE(disabled.device_extensions.empty());

    SECTION("advertised BDA alone is not treated as enabled") {
        unavailable.buffer_device_address_feature = true;
        const auto partial =
            selectRayQueryDeviceFeatures(unavailable);
        REQUIRE_FALSE(partial.buffer_device_address);
        REQUIRE(partial.device_extensions.empty());
        const auto allocator_flags =
            selectVmaAllocatorCreateFlags(
                false, partial.buffer_device_address);
        REQUIRE_FALSE(static_cast<bool>(
            allocator_flags &
            vma::AllocatorCreateFlagBits::
                eBufferDeviceAddress));
    }

    SECTION("a missing dependency prevents every partial extension set") {
        const RayQueryDeviceSupport partial{
            .acceleration_structure_feature = true,
            .ray_query_feature = true,
            .buffer_device_address_feature = true,
            .acceleration_structure_extension = true,
            .ray_query_extension = true,
            .deferred_host_operations_extension = false,
            .min_acceleration_structure_scratch_offset_alignment =
                256,
        };
        const auto selection =
            selectRayQueryDeviceFeatures(partial);
        REQUIRE_FALSE(selection.acceleration_structure);
        REQUIRE_FALSE(selection.ray_query);
        REQUIRE_FALSE(selection.buffer_device_address);
        REQUIRE(selection.device_extensions.empty());
    }

    SECTION("the complete dependency set is enabled together") {
        const RayQueryDeviceSupport supported{
            .acceleration_structure_feature = true,
            .ray_query_feature = true,
            .buffer_device_address_feature = true,
            .acceleration_structure_extension = true,
            .ray_query_extension = true,
            .deferred_host_operations_extension = true,
            .min_acceleration_structure_scratch_offset_alignment =
                256,
        };
        const auto selection =
            selectRayQueryDeviceFeatures(supported);
        REQUIRE(selection.acceleration_structure);
        REQUIRE(selection.ray_query);
        REQUIRE(selection.buffer_device_address);
        REQUIRE(selection
                    .min_acceleration_structure_scratch_offset_alignment ==
                256);
        REQUIRE(selection.device_extensions.size() == 3);
        REQUIRE(hasExtension(
            selection,
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME));
        REQUIRE(hasExtension(
            selection, VK_KHR_RAY_QUERY_EXTENSION_NAME));
        REQUIRE(hasExtension(
            selection,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME));
    }

    SECTION("an incomplete ray tracing pipeline contract stays disabled") {
        const RayQueryDeviceSupport supported{
            .acceleration_structure_feature = true,
            .ray_query_feature = true,
            .buffer_device_address_feature = true,
            .acceleration_structure_extension = true,
            .ray_query_extension = true,
            .deferred_host_operations_extension = true,
            .ray_tracing_pipeline_feature = true,
            .ray_tracing_pipeline_extension = true,
            .ray_tracing_indirect_feature = true,
            .shader_group_handle_size = 32,
            .shader_group_base_alignment = 64,
            .shader_group_handle_alignment = 0,
            .ray_tracing_facts = {
                .max_ray_recursion_depth = 8,
                .max_ray_hit_attribute_size = 32,
                .max_ray_dispatch_invocation_count = 4096,
                .max_dispatch_dimensions = {64, 64, 1},
                .device_name = "must be zeroed",
            },
        };
        const auto selection =
            selectRayQueryDeviceFeatures(supported);
        CHECK(selection.ray_query);
        CHECK_FALSE(selection.ray_tracing_pipeline);
        CHECK_FALSE(selection.ray_tracing_indirect);
        CHECK(selection.ray_tracing_facts ==
              RayTracingRuntimeFacts{});
        CHECK_FALSE(hasExtension(
            selection,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));
    }

    SECTION("the complete ray tracing pipeline contract is atomic") {
        const RayQueryDeviceSupport supported{
            .acceleration_structure_feature = true,
            .ray_query_feature = true,
            .buffer_device_address_feature = true,
            .acceleration_structure_extension = true,
            .ray_query_extension = true,
            .deferred_host_operations_extension = true,
            .ray_tracing_pipeline_feature = true,
            .ray_tracing_pipeline_extension = true,
            .shader_group_handle_size = 32,
            .shader_group_base_alignment = 64,
            .shader_group_handle_alignment = 32,
            .ray_tracing_facts = {
                .max_ray_recursion_depth = 8,
                .max_ray_hit_attribute_size = 32,
                .max_ray_dispatch_invocation_count = 4096,
                .max_dispatch_dimensions = {64, 64, 1},
                .device_name = "WP364 policy fixture",
            },
        };
        const auto selection =
            selectRayQueryDeviceFeatures(supported);
        CHECK(selection.ray_tracing_pipeline);
        CHECK_FALSE(selection.ray_tracing_indirect);
        CHECK(selection.shader_group_handle_size == 32);
        CHECK(selection.shader_group_base_alignment == 64);
        CHECK(selection.shader_group_handle_alignment == 32);
        CHECK(selection.ray_tracing_facts ==
              supported.ray_tracing_facts);
        CHECK(hasExtension(
            selection,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));
        auto indirect_supported = supported;
        indirect_supported.ray_tracing_indirect_feature = true;
        const auto indirect_selection =
            selectRayQueryDeviceFeatures(indirect_supported);
        CHECK(indirect_selection.ray_tracing_pipeline);
        CHECK(indirect_selection.ray_tracing_indirect);
        CHECK(indirect_selection.device_extensions ==
              selection.device_extensions);
    }
}

TEST_CASE("VMA buffer device address flag exactly follows logical-device enablement",
          "[vulkan][ray-query][vma]") {
    for (const bool memory_budget : {false, true}) {
        for (const bool buffer_device_address : {false, true}) {
            const auto flags = selectVmaAllocatorCreateFlags(
                memory_budget, buffer_device_address);
            REQUIRE(static_cast<bool>(
                        flags &
                        vma::AllocatorCreateFlagBits::
                            eExtMemoryBudget) ==
                    memory_budget);
            REQUIRE(static_cast<bool>(
                        flags &
                        vma::AllocatorCreateFlagBits::
                            eBufferDeviceAddress) ==
                    buffer_device_address);
        }
    }
}

TEST_CASE(
    "WP368 bindless material device policy fixes all features and limits",
    "[WP368][vulkan][ray-tracing][bindless][capability][mutation]") {
    using Catch::Matchers::ContainsSubstring;
    RayQueryDeviceSupport support{
        .runtime_descriptor_array_feature = true,
        .shader_sampled_image_array_non_uniform_indexing_feature = true,
        .descriptor_binding_partially_bound_feature = true,
        .max_bound_descriptor_sets = 4,
        .max_descriptor_set_samplers = 1026,
        .max_descriptor_set_sampled_images = 1026,
        .max_per_stage_descriptor_samplers = 1026,
        .max_per_stage_descriptor_sampled_images = 1026,
        .max_per_stage_resources = 1035,
        .device_name = "WP368 fixture device",
    };
    const auto selected = selectRayQueryDeviceFeatures(support);
    CHECK(selected.runtime_descriptor_array);
    CHECK(selected.shader_sampled_image_array_non_uniform_indexing);
    CHECK(selected.descriptor_binding_partially_bound);
    CHECK(selected.ray_material_bindless);
    CHECK(selected.max_bound_descriptor_sets == 4);
    CHECK(selected.max_descriptor_set_samplers == 1026);
    CHECK(selected.max_descriptor_set_sampled_images == 1026);
    CHECK(selected.max_per_stage_descriptor_samplers == 1026);
    CHECK(selected.max_per_stage_descriptor_sampled_images == 1026);
    CHECK(selected.max_per_stage_resources == 1035);
    CHECK(selected.device_name == "WP368 fixture device");

    ShaderReflection reflected_sets{
        .bindings = {
            {1, 0, vk::DescriptorType::eStorageImage, 1,
             vk::ShaderStageFlagBits::eRaygenKHR, "output"},
            {2, 0, vk::DescriptorType::eCombinedImageSampler, 2,
             vk::ShaderStageFlagBits::eClosestHitKHR, "surfaces"},
        },
    };
    auto actual_bindings = frameDescriptorSetLayoutBindings(true);
    for (const auto set : {1u, 2u}) {
        const auto reflected = makeDescriptorSetLayoutBindings(
            reflected_sets, set);
        actual_bindings.insert(actual_bindings.end(),
                               reflected.begin(), reflected.end());
    }
    const auto set_three = rayMaterialDescriptorSetLayoutBindings();
    actual_bindings.insert(actual_bindings.end(),
                           set_three.begin(), set_three.end());
    const auto requirements = rayMaterialBindlessLayoutRequirements(
        4, actual_bindings);
    const RayMaterialBindlessLayoutRequirements expected_requirements{
        .descriptor_set_count = 4,
        .descriptor_set_samplers = 1026,
        .descriptor_set_sampled_images = 1026,
        .per_stage_descriptor_samplers = 1026,
        .per_stage_descriptor_sampled_images = 1026,
        .per_stage_resources = 1035,
    };
    CHECK(requirements == expected_requirements);

    VulkanRuntimeCapabilities capabilities{
        .runtime_descriptor_array = true,
        .shader_sampled_image_array_non_uniform_indexing = true,
        .descriptor_binding_partially_bound = true,
        .ray_material_bindless = true,
        .max_bound_descriptor_sets = 4,
        .max_descriptor_set_samplers = 1026,
        .max_descriptor_set_sampled_images = 1026,
        .max_per_stage_descriptor_samplers = 1026,
        .max_per_stage_descriptor_sampled_images = 1026,
        .max_per_stage_resources = 1035,
        .device_name = "WP368 fixture device",
    };
    CHECK_NOTHROW(requireRayMaterialBindlessDevice(capabilities));
    CHECK_NOTHROW(requireRayMaterialBindlessDevice(
        capabilities, 4, actual_bindings));

    struct FeatureMutation {
        const char *property;
        bool VulkanRuntimeCapabilities::*member;
    };
    constexpr std::array feature_mutations{
        FeatureMutation{"runtimeDescriptorArray",
                        &VulkanRuntimeCapabilities::runtime_descriptor_array},
        FeatureMutation{
            "shaderSampledImageArrayNonUniformIndexing",
            &VulkanRuntimeCapabilities::
                shader_sampled_image_array_non_uniform_indexing},
        FeatureMutation{
            "descriptorBindingPartiallyBound",
            &VulkanRuntimeCapabilities::descriptor_binding_partially_bound},
    };
    for (const auto &mutation : feature_mutations) {
        auto missing = capabilities;
        missing.*(mutation.member) = false;
        CAPTURE(mutation.property);
        CHECK_THROWS_WITH(
            requireRayMaterialBindlessDevice(missing),
            ContainsSubstring(mutation.property) &&
                ContainsSubstring("required=VK_TRUE") &&
                ContainsSubstring("actual=VK_FALSE") &&
                ContainsSubstring("WP368 fixture device"));
    }

    struct LimitMutation {
        const char *property;
        std::uint32_t VulkanRuntimeCapabilities::*member;
        std::uint32_t required;
    };
    constexpr std::array limit_mutations{
        LimitMutation{"maxBoundDescriptorSets",
                      &VulkanRuntimeCapabilities::max_bound_descriptor_sets,
                      4},
        LimitMutation{"maxDescriptorSetSamplers",
                      &VulkanRuntimeCapabilities::max_descriptor_set_samplers,
                      1026},
        LimitMutation{
            "maxDescriptorSetSampledImages",
            &VulkanRuntimeCapabilities::max_descriptor_set_sampled_images,
            1026},
        LimitMutation{
            "maxPerStageDescriptorSamplers",
            &VulkanRuntimeCapabilities::max_per_stage_descriptor_samplers,
            1026},
        LimitMutation{
            "maxPerStageDescriptorSampledImages",
            &VulkanRuntimeCapabilities::max_per_stage_descriptor_sampled_images,
            1026},
        LimitMutation{"maxPerStageResources",
                      &VulkanRuntimeCapabilities::max_per_stage_resources,
                      1035},
    };
    for (const auto &mutation : limit_mutations) {
        auto too_small = capabilities;
        too_small.*(mutation.member) = mutation.required - 1;
        CAPTURE(mutation.property, mutation.required);
        CHECK_THROWS_WITH(
            requireRayMaterialBindlessDevice(
                too_small, 4, actual_bindings),
            ContainsSubstring(mutation.property) &&
                ContainsSubstring("required>=" +
                                  std::to_string(mutation.required)) &&
                ContainsSubstring(
                    "actual=" + std::to_string(mutation.required - 1)) &&
                ContainsSubstring("WP368 fixture device"));
    }

    SECTION("removing any one advertised feature disables the atomic contract") {
        for (const auto feature : {0, 1, 2}) {
            auto mutant = support;
            if (feature == 0) mutant.runtime_descriptor_array_feature = false;
            if (feature == 1) {
                mutant.shader_sampled_image_array_non_uniform_indexing_feature =
                    false;
            }
            if (feature == 2) {
                mutant.descriptor_binding_partially_bound_feature = false;
            }
            const auto rejected = selectRayQueryDeviceFeatures(mutant);
            CAPTURE(feature);
            CHECK_FALSE(rejected.runtime_descriptor_array);
            CHECK_FALSE(
                rejected.shader_sampled_image_array_non_uniform_indexing);
            CHECK_FALSE(rejected.descriptor_binding_partially_bound);
            CHECK_FALSE(rejected.ray_material_bindless);
        }
    }

    SECTION("removing any one advertised descriptor limit disables the atomic contract") {
        struct SupportLimitMutation {
            std::uint32_t RayQueryDeviceSupport::*member;
            std::uint32_t below_minimum;
        };
        constexpr std::array mutations{
            SupportLimitMutation{
                &RayQueryDeviceSupport::max_bound_descriptor_sets, 3},
            SupportLimitMutation{
                &RayQueryDeviceSupport::max_descriptor_set_samplers, 1023},
            SupportLimitMutation{
                &RayQueryDeviceSupport::max_descriptor_set_sampled_images,
                1023},
            SupportLimitMutation{
                &RayQueryDeviceSupport::max_per_stage_descriptor_samplers,
                1023},
            SupportLimitMutation{
                &RayQueryDeviceSupport::max_per_stage_descriptor_sampled_images,
                1023},
            SupportLimitMutation{
                &RayQueryDeviceSupport::max_per_stage_resources, 1025},
        };
        for (const auto &mutation : mutations) {
            auto mutant = support;
            mutant.*(mutation.member) = mutation.below_minimum;
            CAPTURE(mutation.below_minimum);
            CHECK_FALSE(
                selectRayQueryDeviceFeatures(mutant).ray_material_bindless);
        }
    }
}

} // namespace Pelican
