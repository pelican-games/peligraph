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
    }
    throw std::runtime_error("unknown shader stage");
}

std::string shaderStageName(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::vertex:
        return "vertex";
    case ShaderStage::fragment:
        return "fragment";
    }
    throw std::runtime_error("unknown shader stage");
}

bool hasKnownShaderExtension(std::string_view ref) {
    const auto ext = lowerExtension(ref);
    return ext == ".spv" || ext == ".vert" || ext == ".frag" || ext == ".wgsl";
}

ShaderReference makeShaderReference(std::string ref, ShaderStage stage) {
    const bool explicit_file = hasKnownShaderExtension(ref);
    return ShaderReference{
        std::move(ref),
        stage,
        explicit_file ? ShaderReferenceKind::explicit_file : ShaderReferenceKind::stem,
        explicit_file,
    };
}

} // namespace Pelican
