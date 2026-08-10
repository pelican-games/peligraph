#include "../src/core/vkcore/devicefeaturepolicy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

namespace Pelican {
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
            .shader_group_handle_size = 32,
            .shader_group_base_alignment = 64,
            .shader_group_handle_alignment = 0,
        };
        const auto selection =
            selectRayQueryDeviceFeatures(supported);
        CHECK(selection.ray_query);
        CHECK_FALSE(selection.ray_tracing_pipeline);
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
        };
        const auto selection =
            selectRayQueryDeviceFeatures(supported);
        CHECK(selection.ray_tracing_pipeline);
        CHECK(selection.shader_group_handle_size == 32);
        CHECK(selection.shader_group_base_alignment == 64);
        CHECK(selection.shader_group_handle_alignment == 32);
        CHECK(hasExtension(
            selection,
            VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));
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

} // namespace Pelican
