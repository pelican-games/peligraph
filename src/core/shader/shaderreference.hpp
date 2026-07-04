#pragma once

#include <string>
#include <string_view>

namespace Pelican {

enum class ShaderStage {
    vertex,
    fragment,
    compute,
};

enum class ShaderReferenceKind {
    explicit_file,
    stem,
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
ShaderReference makeShaderReference(std::string ref, ShaderStage stage);

} // namespace Pelican
