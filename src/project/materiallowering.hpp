#pragma once

#include "materialformat.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

inline constexpr std::uint32_t materialCustomTextureFirstBinding = 7;
inline constexpr std::size_t materialCustomValueCapacity = 256;

struct Std140MemberLayout {
    std::string name;
    SurfaceParamType type = SurfaceParamType::floating;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::size_t alignment = 0;
};

struct Std140Layout {
    std::vector<Std140MemberLayout> members;
    std::size_t size = 0;
    std::size_t alignment = 16;
};

enum class LoweredTextureView {
    srgb,
    unorm,
};

enum class MaterialDummyTexture {
    white,
    flat_normal,
    black,
};

struct LoweredTextureBinding {
    std::string name;
    std::uint32_t binding = materialCustomTextureFirstBinding;
    std::string reference;
    SurfaceTextureRole role = SurfaceTextureRole::data;
    LoweredTextureView view = LoweredTextureView::unorm;
    MaterialDummyTexture missing_default = MaterialDummyTexture::white;
};

struct MaterialLoweringCapabilities {
    std::size_t max_custom_textures = 25;
    bool color_attachment_blend = true;
    bool depth_attachment = true;
};

struct DeferredEligibility {
    bool compatible = false;
    DeferredMaterialModel model = DeferredMaterialModel::standard_pbr_v1;
    std::string reason;
};

struct LoweredMaterial {
    std::string name;
    std::string surface;
    std::vector<std::string> defines;
    Std140Layout values_layout;
    std::vector<std::byte> values;
    std::vector<LoweredTextureBinding> textures;
    std::vector<std::string> screen_inputs;
    MaterialRouteClass route = MaterialRouteClass::deferred_geometry;
    MaterialRouteReason route_reason = MaterialRouteReason::automatic_deferred_compatible;
    DeferredEligibility deferred_eligibility;
    std::optional<std::string> exact_pass;
    // Compatibility projection retained for PlanViewer and older tooling.
    // It is the route name, not a globally unique runtime PassId.
    std::string target_pass;
    SurfaceRenderState render_state;
    SurfaceHookSet hooks;
    std::optional<MaterialVariantRouting> routing;
};

DeferredEligibility evaluateDeferredEligibility(
    const MaterialDefinition &material,
    const SurfaceFormatDocument &surface);

Std140Layout makeSurfaceStd140Layout(const SurfaceFormatDocument &surface);

std::vector<std::byte> bindSurfaceValues(const SurfaceFormatDocument &surface,
                                         std::span<const MaterialValue> overrides = {},
                                         std::string_view material_name = "<surface-defaults>");

void validateSurfaceCapabilities(const SurfaceFormatDocument &surface,
                                 const MaterialLoweringCapabilities &capabilities,
                                 std::string_view material_name);

LoweredMaterial lowerMaterial(const MaterialDefinition &material,
                              const SurfaceFormatDocument &surface,
                              const MaterialLoweringCapabilities &capabilities = {});

LoweredMaterial lowerMaterialWithSnapshots(
    const MaterialDefinition &material,
    const SurfaceFormatDocument &surface,
    std::span<const std::string> available_snapshots,
    const MaterialLoweringCapabilities &capabilities = {});

LoweredMaterial lowerSurfaceDefaults(const SurfaceFormatDocument &surface,
                                     std::string_view surface_name,
                                     const MaterialLoweringCapabilities &capabilities = {});

std::string dumpLoweredMaterial(const LoweredMaterial &material);

} // namespace Pelican
