#include "shadersourceresolver.hpp"

#include "projectpathresolver.hpp"

#include <system_error>
#include <stdexcept>
#include <variant>

namespace Pelican {
namespace {

constexpr std::string_view engine_scheme = "engine://";
constexpr std::string_view generated_scheme = "generated://";
constexpr std::string_view project_scheme = "project://";
constexpr std::string_view user_scheme = "user://";

std::string canonicalLogicalSourceReference(std::string_view reference) {
    if (reference.starts_with(user_scheme)) {
        const auto relative = std::filesystem::path{
            std::string{reference.substr(user_scheme.size())}}
                                  .lexically_normal()
                                  .generic_string();
        return std::string{user_scheme} + relative;
    }

    auto relative = reference;
    if (relative.starts_with(project_scheme)) {
        relative.remove_prefix(project_scheme.size());
    }
    const auto normalized =
        std::filesystem::path{std::string{relative}}
            .lexically_normal()
            .generic_string();
    return std::string{project_scheme} + normalized;
}

const std::filesystem::path *resolvedPath(const ResolvedRef &reference) {
    if (const auto *path =
            std::get_if<std::filesystem::path>(&reference)) {
        return path;
    }
    if (const auto *fragment =
            std::get_if<ResolvedPathFragment>(&reference)) {
        return &fragment->path;
    }
    return nullptr;
}

} // namespace

std::string_view shaderSourceStageExtension(ShaderSourceStage stage) {
    switch (stage) {
    case ShaderSourceStage::vertex:
        return ".vert";
    case ShaderSourceStage::fragment:
        return ".frag";
    case ShaderSourceStage::compute:
        return ".comp";
    case ShaderSourceStage::raygen:
        return ".rgen";
    case ShaderSourceStage::miss:
        return ".rmiss";
    case ShaderSourceStage::closesthit:
        return ".rchit";
    }
    throw std::runtime_error("unknown shader source stage");
}

std::string shaderSourceCandidateReference(
    std::string_view stem, ShaderSourceStage stage, bool spirv) {
    std::string result{stem};
    result += shaderSourceStageExtension(stage);
    if (spirv) {
        result += ".spv";
    }
    return result;
}

std::string_view shaderSourceOpenReasonName(
    ShaderSourceOpenReason reason) {
    switch (reason) {
    case ShaderSourceOpenReason::embedded_engine_resource:
        return "embedded_engine_resource";
    case ShaderSourceOpenReason::generated:
        return "generated";
    case ShaderSourceOpenReason::source_not_found:
        return "source_not_found";
    }
    throw std::runtime_error("unknown shader source open reason");
}

bool isCanonicalShaderSourceOpenReference(
    std::string_view logical_ref) {
    std::string_view relative;
    if (logical_ref.starts_with(project_scheme)) {
        relative = logical_ref.substr(project_scheme.size());
    } else if (logical_ref.starts_with(user_scheme)) {
        relative = logical_ref.substr(user_scheme.size());
    } else {
        return false;
    }
    if (relative.empty() ||
        relative.find('\\') != std::string_view::npos ||
        relative.find('#') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path path{std::string{relative}};
    if (path.is_absolute() || path.has_root_name() ||
        path.has_root_directory()) {
        return false;
    }
    const auto normalized = path.lexically_normal().generic_string();
    if (normalized != relative || normalized == ".") {
        return false;
    }
    const auto extension = path.extension().generic_string();
    return extension == ".vert" || extension == ".frag" ||
           extension == ".comp" || extension == ".rgen" ||
           extension == ".rmiss" || extension == ".rchit";
}

bool isCanonicalShaderSourceOpenReference(
    std::string_view logical_ref, ShaderSourceStage stage) {
    if (!isCanonicalShaderSourceOpenReference(logical_ref)) {
        return false;
    }
    const auto scheme_end = logical_ref.find("://");
    const auto relative = logical_ref.substr(scheme_end + 3);
    return std::filesystem::path{std::string{relative}}
               .extension()
               .generic_string() == shaderSourceStageExtension(stage);
}

ShaderSourceOpenResolution resolveShaderSourceOpenReference(
    const ProjectPathResolver &resolver, std::string_view stem,
    ShaderSourceStage stage,
    const EngineShaderSourceExists &engine_source_exists) {
    const auto source_ref =
        shaderSourceCandidateReference(stem, stage, false);
    if (source_ref.starts_with(engine_scheme)) {
        const auto id =
            std::string_view{source_ref}.substr(engine_scheme.size());
        if (engine_source_exists && !engine_source_exists(id)) {
            return ShaderSourceOpenResolution{
                .reason = ShaderSourceOpenReason::source_not_found,
            };
        }
        return ShaderSourceOpenResolution{
            .reason =
                ShaderSourceOpenReason::embedded_engine_resource,
        };
    }
    if (source_ref.starts_with(generated_scheme)) {
        return ShaderSourceOpenResolution{
            .reason = ShaderSourceOpenReason::generated,
        };
    }

    const auto resolved = resolver.resolveProjectRef(source_ref).reference;
    if (std::holds_alternative<EngineResourceId>(resolved) ||
        std::holds_alternative<ResolvedEngineFragment>(resolved)) {
        const auto &id = std::holds_alternative<EngineResourceId>(resolved)
                             ? std::get<EngineResourceId>(resolved).id
                             : std::get<ResolvedEngineFragment>(resolved)
                                   .resource.id;
        if (engine_source_exists && !engine_source_exists(id)) {
            return ShaderSourceOpenResolution{
                .reason = ShaderSourceOpenReason::source_not_found,
            };
        }
        return ShaderSourceOpenResolution{
            .reason =
                ShaderSourceOpenReason::embedded_engine_resource,
        };
    }
    if (std::holds_alternative<ResolvedPathFragment>(resolved)) {
        throw std::runtime_error(
            "Shader source references do not support asset fragments: " +
            source_ref);
    }
    const auto *path = resolvedPath(resolved);
    if (path == nullptr) {
        throw std::runtime_error(
            "Shader source reference resolved to an unsupported value: " +
            source_ref);
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(*path, error) || error) {
        return ShaderSourceOpenResolution{
            .reason = ShaderSourceOpenReason::source_not_found,
        };
    }
    return ShaderSourceOpenResolution{
        .logical_ref =
            canonicalLogicalSourceReference(source_ref),
        .physical_path = *path,
    };
}

std::filesystem::path materializeShaderSourceOpenReference(
    const ProjectPathResolver &resolver, std::string_view logical_ref) {
    if (!isCanonicalShaderSourceOpenReference(logical_ref)) {
        throw std::runtime_error(
            "shader source_open_ref requires a canonical project:// or "
            "user:// reference: " +
            std::string{logical_ref});
    }
    return resolver.resolveExistingFile(logical_ref);
}

} // namespace Pelican
