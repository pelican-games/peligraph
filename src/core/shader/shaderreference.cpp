#include "shaderreference.hpp"

#include "../../project/shadersourceresolver.hpp"

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
    }
    throw std::runtime_error("unknown shader stage");
}

bool hasKnownShaderExtension(std::string_view ref) {
    const auto ext = lowerExtension(ref);
    return ext == ".spv" || ext == ".vert" || ext == ".frag" ||
           ext == ".comp" || ext == ".rgen" || ext == ".rmiss" ||
           ext == ".rchit" || ext == ".rahit" || ext == ".rint" ||
           ext == ".wgsl";
}

bool hasKnownShaderExtension(
    std::string_view ref, ShaderStage stage) {
    const auto ext = lowerExtension(ref);
    // .rahit became vocabulary in RT2a.  Before that addition it was a
    // perfectly valid final component of an arbitrary stem (for example a
    // closest-hit stem "foo.rahit" resolved to foo.rahit.rchit).  Reserve
    // the new suffix only in the stage that owns it so old tasks keep that
    // resolution behavior.  All older explicit suffixes retain their
    // historical, stage-independent rejection.
    if (ext == ".rahit") return stage == ShaderStage::anyhit;
    if (ext == ".rint") return stage == ShaderStage::intersection;
    return hasKnownShaderExtension(ref);
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
