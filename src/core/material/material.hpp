#pragma once

#include "../container.hpp"
#include "../handle.hpp"
#include "../shader/shaderlibrary.hpp"
#include "../../project/materiallowering.hpp"
#include <array>
#include <cstddef>
#include <functional>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>
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
    std::vector<std::string> tags;
    bool skinned = false;
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
    struct CustomTextureBinding {
        std::string name;
        std::optional<GlobalTextureId> texture;
        SurfaceTextureRole role = SurfaceTextureRole::data;
        MaterialDummyTexture missing_default = MaterialDummyTexture::white;
        SurfaceTextureDimension dimension =
            SurfaceTextureDimension::two_d;
        SurfaceTextureSampler sampler;
    };
    std::vector<CustomTextureBinding> custom_textures;
    Std140Layout custom_values_layout;
    std::vector<std::byte> custom_values;
    SurfaceRenderState render_state;
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    MaterialShaderContract shader_contract = MaterialShaderContract::gbuffer_v1;
    std::optional<std::string> exact_pass;
    std::vector<MaterialScreenInputContract> screen_inputs;
    std::vector<SurfaceResourcePortDefinition> resource_ports;
};

struct alignas(16) MaterialGpuData {
    alignas(16) glm::vec4 base_color_factor{1.0f};
    alignas(16) glm::vec4 emissive_factor{1.0f};
    alignas(16) glm::vec4 surface_factors{1.0f};
    alignas(16) glm::vec4 vat_bounds_min_frame_count{0.0f};
    alignas(16) glm::vec4 vat_bounds_extent_fps{0.0f};
    alignas(16) glm::ivec4 vat_flags{0};
    // std140-packed custom values. Generated C/B shims address these words by
    // the offsets produced by materiallowering. Existing PBR materials leave
    // the storage zeroed and retain their original fields verbatim.
    alignas(16) std::array<std::uint32_t, materialCustomValueCapacity / 4> custom_values{};
};

static_assert(sizeof(MaterialGpuData) == 96 + materialCustomValueCapacity);

// Applies the engine-independent lowering result to the GPU registration
// request. Texture slots initially use their semantic dummy and may be
// replaced by the loader before registerMaterial().
void applyLoweredMaterial(MaterialInfo &destination, const LoweredMaterial &lowered);

using LoweredMaterialTextureResolver =
    std::function<GlobalTextureId(std::string_view reference, SurfaceTextureRole role)>;

// Resolves every declared/default or per-material overridden reference and
// installs the resulting texture handles. The original overload intentionally
// retains the established semantic-dummy behavior.
void applyLoweredMaterial(MaterialInfo &destination, const LoweredMaterial &lowered,
                          const LoweredMaterialTextureResolver &resolve_texture);

// Opt-in route-aware application used by a hybrid pipeline.  The caller must
// compile the surface with surfacePassForMaterialRoute(lowered.route); this
// function selects the matching attachment ABI.  The established overloads
// above retain the legacy five-MRT shader contract.
void applyLoweredMaterialForRoute(MaterialInfo &destination,
                                  const LoweredMaterial &lowered);
void applyLoweredMaterialForRoute(MaterialInfo &destination,
                                  const LoweredMaterial &lowered,
                                  const LoweredMaterialTextureResolver &resolve_texture);

} // namespace Pelican
