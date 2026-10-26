#include "shaderreference.hpp"

#include "shadersourceresolver.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace Pelican {

std::string_view shaderStageSourceExtension(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::vertex:
        return shaderSourceStageExtension(
            ShaderSourceStage::vertex);
    case ShaderStage::fragment:
        return shaderSourceStageExtension(
            ShaderSourceStage::fragment);
    case ShaderStage::compute:
        return shaderSourceStageExtension(
            ShaderSourceStage::compute);
    case ShaderStage::raygen:
        return shaderSourceStageExtension(
            ShaderSourceStage::raygen);
    case ShaderStage::miss:
        return shaderSourceStageExtension(
            ShaderSourceStage::miss);
    case ShaderStage::closesthit:
        return shaderSourceStageExtension(
            ShaderSourceStage::closesthit);
    case ShaderStage::anyhit:
        return shaderSourceStageExtension(ShaderSourceStage::anyhit);
    case ShaderStage::intersection:
        return shaderSourceStageExtension(ShaderSourceStage::intersection);
    case ShaderStage::callable:
        return shaderSourceStageExtension(ShaderSourceStage::callable);
    }
    throw std::runtime_error("unknown shader stage");
}

std::string shaderStageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::vertex:
        return "vertex";
    case ShaderStage::fragment:
        return "fragment";
    case ShaderStage::compute:
        return "compute";
    case ShaderStage::raygen:
        return "raygen";
    case ShaderStage::miss:
        return "miss";
    case ShaderStage::closesthit:
        return "closesthit";
    case ShaderStage::anyhit:
        return "anyhit";
    case ShaderStage::intersection:
        return "intersection";
    case ShaderStage::callable:
        return "callable";
    }
    throw std::runtime_error("unknown shader stage");
}

bool hasKnownShaderExtension(std::string_view ref) {
    return hasKnownShaderSourceExtension(ref);
}

bool hasKnownShaderExtension(std::string_view ref, ShaderStage stage) {
    if (stage == ShaderStage::anyhit) return hasKnownShaderSourceExtension(ref, ShaderSourceStage::anyhit);
    if (stage == ShaderStage::intersection) return hasKnownShaderSourceExtension(ref, ShaderSourceStage::intersection);
    return hasKnownShaderSourceExtension(ref, ShaderSourceStage::vertex);
}

ShaderReference makeShaderReference(std::string ref, ShaderStage stage) {
    if (hasKnownShaderExtension(ref, stage)) {
        throw std::runtime_error("shader reference '" + ref +
                                 "' uses an explicit file extension; use an extensionless " +
                                 shaderStageName(stage) + " shader stem");
    }
    return ShaderReference{
        std::move(ref),
        stage,
        ShaderReferenceKind::stem,
        false,
    };
}

} // namespace Pelican
