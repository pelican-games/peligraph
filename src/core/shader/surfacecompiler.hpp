#pragma once

#include "shadercompiler.hpp"
#include "shaderresourceinterface.hpp"
#include "spvlink.hpp"
#include "../../project/materialoutput.hpp"
#include "../../project/renderpipeline.hpp"
#include "../../project/surfaceformat.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

enum class SurfacePass {
    main,
    deferred_geometry,
    forward,
    depth,
    velocity,
};

// Internal physical-lowering defines. Public .surface files keep one logical
// accessor; loadFromSurfaceForMaterial supplies these only after the active
// render graph has selected an input-attachment implementation.
inline constexpr std::string_view
    surfaceScreenInputLocalReadDefinePrefix =
        "PELICAN_MATERIAL_SCREEN_INPUT_";
inline constexpr std::string_view
    surfaceResourceLocalReadDefinePrefix =
        "PELICAN_MATERIAL_RESOURCE_";

std::string makeSurfaceScreenInputLocalReadDefine(
    std::size_t input,
    std::uint32_t input_attachment_index);
std::string makeSurfaceResourceLocalReadDefine(
    std::size_t resource,
    std::uint32_t input_attachment_index);
std::string makeSurfaceResourceLayeredDefine(
    std::size_t resource);
std::string makeSurfaceResourceCubeDefine(
    std::size_t resource);

struct SurfaceShaderComposition {
    std::string vertex_source;
    std::string fragment_source;
    std::vector<std::pair<std::string, std::string>> virtual_includes;
    std::vector<std::string> defines;
    std::vector<ShaderResourceInterfaceBinding>
        resource_interface;
    std::optional<MaterialOutputSchema>
        material_output_schema;
};

struct SurfaceCompileResult {
    ShaderCompileResult vertex;
    ShaderCompileResult fragment;
    std::vector<SpvLinkBinding> vertex_bindings;
    std::vector<SpvLinkBinding> fragment_bindings;
    std::string vertex_cache_key;
    std::string fragment_cache_key;
    bool experimental_spv_link = false;
};

SurfaceShaderComposition composeSurfaceShaders(const SurfaceFormatDocument &surface,
                                                std::string_view source_name,
                                                SurfacePass pass = SurfacePass::main,
                                                std::vector<std::string> defines = {},
                                                std::optional<MaterialOutputSchema>
                                                    material_output_schema =
                                                        std::nullopt);

SurfaceCompileResult compileSurfaceShaders(ShaderCompiler &compiler,
                                           const SurfaceFormatDocument &surface,
                                           std::string_view source_name,
                                           SurfacePass pass = SurfacePass::main,
                                           std::vector<std::string> defines = {},
                                           std::optional<MaterialOutputSchema>
                                               material_output_schema =
                                                   std::nullopt);

std::string_view surfacePassName(SurfacePass pass);
SurfacePass surfacePassForMaterialRoute(MaterialRouteClass route);

// Selection is deliberately process-explicit and defaults to false.  Merely
// linking SPIRV-Tools into the engine never changes the WP78 source path.
bool surfaceSpvLinkExperimentalEnabled();

} // namespace Pelican
