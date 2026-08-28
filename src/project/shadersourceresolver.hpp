#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Pelican {

class ProjectPathResolver;

// Source-stage naming is project-format data.  Keeping this table in
// pelican_project lets the engine and Studio derive exactly the same source
// filename without either side duplicating backend code.
enum class ShaderSourceStage {
    vertex,
    fragment,
    compute,
    raygen,
    miss,
    closesthit,
};

enum class ShaderSourceOpenReason {
    embedded_engine_resource,
    generated,
    source_not_found,
};

struct ShaderSourceOpenResolution {
    std::optional<std::string> logical_ref;
    std::optional<std::filesystem::path> physical_path;
    std::optional<ShaderSourceOpenReason> reason;

    [[nodiscard]] bool openable() const noexcept {
        return logical_ref.has_value() && physical_path.has_value() &&
               !reason.has_value();
    }
};

using EngineShaderSourceExists =
    std::function<bool(std::string_view)>;

std::string_view shaderSourceStageExtension(ShaderSourceStage stage);
std::string shaderSourceCandidateReference(
    std::string_view stem, ShaderSourceStage stage, bool spirv);
std::string_view shaderSourceOpenReasonName(ShaderSourceOpenReason reason);

// True only for the portable, lexically-normal project:// or user:// wire
// form with one of the closed stage-source suffixes. These checks do not
// touch the filesystem; the overload also requires the suffix for `stage`.
bool isCanonicalShaderSourceOpenReference(std::string_view logical_ref);
bool isCanonicalShaderSourceOpenReference(
    std::string_view logical_ref, ShaderSourceStage stage);

// Resolves the source (never the .spv sidecar) and returns a machine-neutral
// reference for the wire.  Project asset-store overrides affect only
// physical_path; logical_ref remains project://<declared mount>/... .
ShaderSourceOpenResolution resolveShaderSourceOpenReference(
    const ProjectPathResolver &resolver, std::string_view stem,
    ShaderSourceStage stage,
    const EngineShaderSourceExists &engine_source_exists = {});

// Studio's inverse operation for a non-null source_open_ref.  Only logical
// project:// and user:// references are accepted; absolute paths can never
// enter through this seam.
std::filesystem::path materializeShaderSourceOpenReference(
    const ProjectPathResolver &resolver, std::string_view logical_ref);

} // namespace Pelican
