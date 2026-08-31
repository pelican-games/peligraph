#pragma once

#include "materialformat.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace Pelican {

inline constexpr std::string_view
    gltfCoreOpenPbrSurfaceDefine =
        "PELICAN_OPENPBR_GLTF_CORE_V1";

struct GltfCoreMaterialDescription {
    std::string name;
    MaterialBase base;
    std::string alpha_mode = "OPAQUE";
    double alpha_cutoff = 0.5;
    bool double_sided = false;
};

// KHR_texture_transform is deliberately not part of the raster material
// contract yet.  Keep its authored state in the normalized alpha IR so the
// portable ray profile can distinguish an absent extension from an authored
// identity, a non-identity transform, and malformed extension data.
enum class NormalizedAlphaUvTransformState : std::uint8_t {
    absent,
    identity,
    non_identity,
    invalid,
};

// The sole normalized alpha representation consumed by the portable ray
// material profile.  Construction is intentionally confined to the project
// lowering function below: runtime handles and resolved filesystem paths are
// not part of its input domain.
class NormalizedAlphaIr {
    friend NormalizedAlphaIr normalizeGltfBaseColorAlpha(
        struct GltfBaseColorAlphaDescription);

    std::string material_identity_;
    std::string texture_identity_;
    MaterialAlphaMode alpha_mode_ = MaterialAlphaMode::opaque;
    float cutoff_ = 0.5F;
    float base_factor_a_ = 1.0F;
    std::array<float, 4> uv_offset_scale_{0.0F, 0.0F, 1.0F, 1.0F};
    float uv_rotation_ = 0.0F;
    NormalizedAlphaUvTransformState uv_transform_state_ =
        NormalizedAlphaUvTransformState::absent;
    std::string digest_sha256_;

  public:
    NormalizedAlphaIr(const NormalizedAlphaIr &) = default;
    NormalizedAlphaIr(NormalizedAlphaIr &&) noexcept = default;
    NormalizedAlphaIr &operator=(const NormalizedAlphaIr &) = default;
    NormalizedAlphaIr &operator=(NormalizedAlphaIr &&) noexcept = default;

    std::string_view sourceKind() const noexcept {
        return "gltf_base_color";
    }
    const std::string &materialIdentity() const noexcept {
        return material_identity_;
    }
    const std::string &textureIdentity() const noexcept {
        return texture_identity_;
    }
    MaterialAlphaMode alphaMode() const noexcept { return alpha_mode_; }
    float cutoff() const noexcept { return cutoff_; }
    float baseFactorA() const noexcept { return base_factor_a_; }
    const std::array<float, 4> &uvOffsetScale() const noexcept {
        return uv_offset_scale_;
    }
    float uvRotation() const noexcept { return uv_rotation_; }
    NormalizedAlphaUvTransformState uvTransformState() const noexcept {
        return uv_transform_state_;
    }
    const std::string &digestSha256() const noexcept {
        return digest_sha256_;
    }

    bool operator==(const NormalizedAlphaIr &) const = default;

  private:
    NormalizedAlphaIr() = default;
};

struct GltfBaseColorAlphaDescription {
    // The WP368a authored model reference before physical path resolution.
    // The normalizer, and no caller, constructs the canonical material and
    // texture identities from this reference and the glTF indices.
    std::string authored_reference;
    std::uint32_t material_index = 0;
    std::optional<std::uint32_t> texture_index;
    MaterialAlphaMode alpha_mode = MaterialAlphaMode::opaque;
    float cutoff = 0.5F;
    float base_factor_a = 1.0F;
    std::array<float, 4> uv_offset_scale{0.0F, 0.0F, 1.0F, 1.0F};
    float uv_rotation = 0.0F;
    NormalizedAlphaUvTransformState uv_transform_state =
        NormalizedAlphaUvTransformState::absent;
};

NormalizedAlphaIr normalizeGltfBaseColorAlpha(
    GltfBaseColorAlphaDescription description);

// Produces the ordinary pelican.material representation used by the material
// lowering pipeline. The glTF loader only transports source fields into this
// adapter; routing and wrapper selection live here.
MaterialDefinition makeGltfCoreMaterialDefinition(
    GltfCoreMaterialDescription description);

} // namespace Pelican
