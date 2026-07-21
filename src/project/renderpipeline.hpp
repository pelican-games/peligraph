#pragma once

#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace Pelican {

// A route is a material classification.  It is deliberately not a pass id:
// several render graphs (flat/XR/preview) may provide different concrete
// passes for the same semantic route.
enum class MaterialRouteClass {
    deferred_geometry,
    forward_opaque,
    forward_transparent,
};

// Fragment-output ABI expected by a material pipeline.
enum class MaterialShaderContract {
    legacy_gbuffer_v1,
    gbuffer_v1,
    forward_scene_color_v1,
};

enum class MaterialPhase {
    opaque,
    transparent,
};

// The pass contract bundles the route accepted by a pass with the attachment
// ABI its material pipelines must use.  The implicit legacy value preserves
// existing five-MRT configs and accepts both generations of the G-buffer ABI.
enum class MaterialPassContract {
    legacy_gbuffer_v1,
    deferred_geometry_v1,
    forward_opaque_v1,
    forward_transparent_v1,
};

enum class MaterialRouteReason {
    automatic_deferred_compatible,
    automatic_openpbr_base,
    automatic_blended,
    automatic_screen_input,
    automatic_custom_brdf,
    automatic_custom_ambient,
    automatic_custom_lighting,
    explicit_deferred,
    explicit_forward,
};

enum class DeferredMaterialModel {
    standard_pbr_v1,
    openpbr_base_v1,
};

std::string_view materialRouteClassName(MaterialRouteClass route);
std::optional<MaterialRouteClass> materialRouteClassFromName(std::string_view name);
std::string_view materialShaderContractName(MaterialShaderContract contract);
std::string_view materialPhaseName(MaterialPhase phase);
std::string_view materialPassContractName(MaterialPassContract contract);
std::optional<MaterialPassContract> materialPassContractFromName(std::string_view name);
std::optional<MaterialRouteClass> materialPassRoute(MaterialPassContract contract);
MaterialShaderContract materialPassShaderContract(MaterialPassContract contract);
MaterialPhase materialPassPhase(MaterialPassContract contract);
std::string_view materialRouteReasonName(MaterialRouteReason reason);
std::string_view deferredMaterialModelName(DeferredMaterialModel model);
bool materialPassAcceptsMaterial(MaterialPassContract pass_contract,
                                 std::string_view pass_name,
                                 MaterialRouteClass material_route,
                                 MaterialShaderContract material_shader_contract,
                                 const std::optional<std::string> &exact_pass = std::nullopt);

struct RenderPipelinePresetInfo {
    std::string reference;
    std::string name;
    int version = 1;
};

struct RenderPipelinePresetResolution {
    nlohmann::json config;
    std::optional<RenderPipelinePresetInfo> preset;
};

using RenderPipelinePresetLoader =
    std::function<std::string(std::string_view reference)>;

// Expands the optional top-level pipeline.preset envelope into an ordinary
// verbose rendering config.  Source files are never rewritten.  v1 permits a
// deliberately small overlay surface; larger customization should copy/eject
// the resolved config instead of depending on an ambiguous deep merge.
RenderPipelinePresetResolution resolveRenderPipelinePreset(
    const nlohmann::json &authored_config,
    const RenderPipelinePresetLoader &load_preset_json = {});

// Validates the optional detailed material_routing table after features have
// been composed.  The returned object is stable diagnostic metadata suitable
// for frame-plan dumps.  A null JSON value means legacy routing.
nlohmann::json resolveMaterialRoutingTable(const nlohmann::json &composed_config);

} // namespace Pelican
