#pragma once

#include <array>
#include <string>
#include <string_view>

namespace Pelican {

enum class ShaderStage {
    vertex,
    fragment,
    compute,
    raygen,
    miss,
    closesthit,
    anyhit,
    intersection,
    callable,
};

// EL-4: every stage, in declaration order. A stem reference loads the stage's
// file <stem><shaderStageSourceExtension> (or its .spv), and a shader path's
// extension names its stage, from this list; get_graph_catalog's
// engine_shaders is read off it.
inline constexpr std::array shaderStages{
    ShaderStage::vertex,     ShaderStage::fragment,     ShaderStage::compute,
    ShaderStage::raygen,     ShaderStage::miss,         ShaderStage::closesthit,
    ShaderStage::anyhit,     ShaderStage::intersection, ShaderStage::callable,
};

enum class ShaderReferenceKind {
    explicit_file,
    stem,
    engine_generated,
};

struct ShaderReference {
    std::string ref;
    ShaderStage stage = ShaderStage::vertex;
    ShaderReferenceKind kind = ShaderReferenceKind::explicit_file;
    bool backend_specific = false;
};

std::string_view shaderStageSourceExtension(ShaderStage stage);
std::string shaderStageName(ShaderStage stage);
bool hasKnownShaderExtension(std::string_view ref);
bool hasKnownShaderExtension(std::string_view ref, ShaderStage stage);
ShaderReference makeShaderReference(std::string ref, ShaderStage stage);

} // namespace Pelican
