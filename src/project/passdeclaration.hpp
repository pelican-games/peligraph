#pragma once

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
