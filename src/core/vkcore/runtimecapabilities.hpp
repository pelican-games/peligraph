#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Pelican {

// The complete production fact set used by RT1a validation.  A device for
// which ray-tracing-pipeline support was not selected carries the all-zero
// value atomically; callers must never assemble a partial fact set.
struct RayTracingRuntimeFacts {
    std::uint32_t max_ray_recursion_depth = 0;
    std::uint32_t max_shader_group_stride = 0;
    std::uint32_t max_ray_hit_attribute_size = 0;
    std::uint64_t max_ray_dispatch_invocation_count = 0;
    std::array<std::uint64_t, 3> max_dispatch_dimensions{};
    std::string device_name;

    bool operator==(const RayTracingRuntimeFacts &) const = default;
};

// Features that were both advertised by the selected physical device and
// enabled on the logical device. Callers must use this runtime contract rather
// than treating an extension name in the Vulkan headers as device support.
struct VulkanRuntimeCapabilities {
    bool timeline_semaphore = false;
    bool multiview = false;
    bool acceleration_structure = false;
    bool ray_query = false;
    bool buffer_device_address = false;
    bool ray_tracing_pipeline = false;
    bool runtime_descriptor_array = false;
    bool shader_sampled_image_array_non_uniform_indexing = false;
    bool descriptor_binding_partially_bound = false;
    bool ray_material_bindless = false;
    std::uint32_t max_bound_descriptor_sets = 0;
    std::uint32_t max_descriptor_set_sampled_images = 0;
    std::string device_name;
    std::uint32_t
        min_acceleration_structure_scratch_offset_alignment = 0;
    std::uint32_t shader_group_handle_size = 0;
    std::uint32_t shader_group_base_alignment = 0;
    std::uint32_t shader_group_handle_alignment = 0;
    RayTracingRuntimeFacts ray_tracing_facts;
    bool dynamic_rendering_local_read = false;
    bool sampler_anisotropy = false;
    bool independent_blend = false;
    bool swapchain_maintenance1 = false;
    bool draw_indirect_count = false;
};

} // namespace Pelican
