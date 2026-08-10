#pragma once

#include <cstdint>

namespace Pelican {

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
    std::uint32_t
        min_acceleration_structure_scratch_offset_alignment = 0;
    std::uint32_t shader_group_handle_size = 0;
    std::uint32_t shader_group_base_alignment = 0;
    std::uint32_t shader_group_handle_alignment = 0;
    bool dynamic_rendering_local_read = false;
    bool sampler_anisotropy = false;
    bool independent_blend = false;
    bool swapchain_maintenance1 = false;
    bool draw_indirect_count = false;
};

} // namespace Pelican
