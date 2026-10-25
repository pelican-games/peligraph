#pragma once

#include <array>
#include <string_view>
#include <cstddef>
#include <optional>
#include <string>

namespace Pelican {

inline constexpr std::size_t maximumPassImplementationProviderNameBytes = 127;

enum class FullscreenPushConstantData {
    eNone,
    eCameraPosition,
    eProjectionView,
};

// Pass defaults shared by IR, execution and authoring. The empty authoring
// spelling preserves omission of push_constants in Studio.
inline constexpr std::array<double, 4> defaultPassClearColor{0.0, 0.0, 0.0, 1.0};
inline constexpr auto defaultFullscreenPushConstants = FullscreenPushConstantData::eNone;
inline constexpr std::string_view defaultFullscreenPushConstantsName = "none";
inline constexpr std::string_view defaultFullscreenPushConstantsAuthoringValue = "";
inline constexpr bool defaultFullscreenUsesLightData = false;
inline constexpr std::string_view defaultShadowDepthShader = "engine://shadow_depth";
inline constexpr std::string_view defaultVelocityShader = "engine://velocity";
inline constexpr std::string_view defaultVelocitySkinnedShader = "engine://velocity_skinned";
inline constexpr std::string_view defaultPickingShader = "engine://picking";
inline constexpr std::string_view defaultPickingSkinnedShader = "engine://picking_skinned";
inline constexpr std::string_view defaultRasterShaderImplementation = "pelican.raster.authored_shader@1";

// Authored stages, before execution supplies any engine defaults. In
// particular, an absent shader member and an empty object are distinct.
struct PassShaderDeclaration {
    bool shader_declared = false;
    std::optional<std::string> vertex_shader;
    std::optional<std::string> skinned_vertex_shader;
    std::optional<std::string> fragment_shader;

    bool operator==(const PassShaderDeclaration &) const = default;
};

} // namespace Pelican
