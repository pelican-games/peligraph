#pragma once

#include "renderingpass.hpp"
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>

namespace Pelican {

struct FrameGraphBufferDefinition;
struct RenderGraphIR;
class RenderTargetMetadataResolver;
class RenderTargetNameResolver;

std::optional<MaterialDrawTagFilter>
parseMaterialDrawTagFilterFromJson(
    const nlohmann::json &pass_json,
    std::string_view context);

std::optional<GpuDrawSourceDeclaration>
parseGpuDrawSourceFromJson(
    const nlohmann::json &pass_json,
    std::string_view context);
// RC-3c2a: every material pass's gpu_draw_source of the IR held to the
// buffers it names (as the generation's resource description sized them):
// the command and count layouts, the ranges inside the buffers, the
// segments' host source.
void validateGpuDrawSourceBufferContracts(
    const RenderGraphIR &render_graph,
    std::span<const FrameGraphBufferDefinition>
        buffer_definitions);

void parseMaterialPassInfoFromJson(PassDefinition &pass_def, const nlohmann::json &pass_json);
void parseMaterialPassScreenInputsFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata);
void parseMaterialPassSurfaceResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const RenderTargetMetadataResolver &rt_metadata);
void parseMaterialPassResourcesFromJson(
    PassDefinition &pass_def, const nlohmann::json &pass_json,
    const RenderTargetNameResolver &rt_resolver,
    const std::unordered_set<std::string> &buffer_names);

} // namespace Pelican
