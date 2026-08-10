#include "shaderreference.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace Pelican {

namespace {

std::string lowerExtension(std::string_view ref) {
    auto ext = std::filesystem::path{std::string{ref}}.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

} // namespace

std::string_view shaderStageSourceExtension(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::vertex:
        return ".vert";
    case ShaderStage::fragment:
        return ".frag";
    case ShaderStage::compute:
        return ".comp";
    case ShaderStage::raygen:
        return ".rgen";
    case ShaderStage::miss:
        return ".rmiss";
    case ShaderStage::closesthit:
        return ".rchit";
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
    }
    throw std::runtime_error("unknown shader stage");
}

bool hasKnownShaderExtension(std::string_view ref) {
    const auto ext = lowerExtension(ref);
    return ext == ".spv" || ext == ".vert" || ext == ".frag" ||
           ext == ".comp" || ext == ".rgen" || ext == ".rmiss" ||
           ext == ".rchit" || ext == ".wgsl";
}

ShaderReference makeShaderReference(std::string ref, ShaderStage stage) {
    if (hasKnownShaderExtension(ref)) {
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
