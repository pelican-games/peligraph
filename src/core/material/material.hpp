#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../shader/shaderlibrary.hpp"
#include <glm/vec3.hpp>
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

} // namespace Pelican
