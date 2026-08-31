#pragma once

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
