#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../shader/shaderlibrary.hpp"
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <span>
#include <vulkan/vulkan.hpp>

namespace Pelican {

PELICAN_DEFINE_HANDLE(GlobalMaterialId, int);
PELICAN_DEFINE_HANDLE(GlobalTextureId, int);

inline constexpr int invalidMaterialIdValue = -1;

inline constexpr GlobalMaterialId invalidMaterialId() {
    return GlobalMaterialId{invalidMaterialIdValue};
}

inline constexpr bool isValidMaterialId(GlobalMaterialId material_id) {
    return material_id.value >= 0;
}

struct MaterialInfo {
    ShaderBundleId vert_shader, frag_shader;
    GlobalTextureId base_color_texture;
    GlobalTextureId metallic_roughness_texture;
    GlobalTextureId normal_texture;
    GlobalTextureId emissive_texture;
    glm::vec4 base_color_factor{1.0f};
    glm::vec3 emissive_factor{1.0f};
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    struct VatPlaybackInfo {
        GlobalTextureId position_texture;
        GlobalTextureId normal_texture;
        glm::vec3 bounds_min{0.0f};
        glm::vec3 bounds_max{0.0f};
        float fps = 0.0f;
        uint32_t frame_count = 0;
        int32_t base_vertex = 0;
        bool loop = false;
        bool has_normal = false;
    };
    std::optional<VatPlaybackInfo> vat;
};

struct alignas(16) MaterialGpuData {
    alignas(16) glm::vec4 base_color_factor{1.0f};
    alignas(16) glm::vec4 emissive_factor{1.0f};
    alignas(16) glm::vec4 surface_factors{1.0f};
    alignas(16) glm::vec4 vat_bounds_min_frame_count{0.0f};
    alignas(16) glm::vec4 vat_bounds_extent_fps{0.0f};
    alignas(16) glm::ivec4 vat_flags{0};
};

static_assert(sizeof(MaterialGpuData) == 96);

} // namespace Pelican
