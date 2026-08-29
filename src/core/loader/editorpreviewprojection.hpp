#pragma once

#include "resolvedscene.hpp"

#include <functional>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Pelican {

enum class EditorPreviewProjectionErrorCode : std::uint8_t {
    schema_violation,
    method_unavailable,
};

std::string_view editorPreviewProjectionErrorCodeName(
    EditorPreviewProjectionErrorCode code) noexcept;

class EditorPreviewProjectionError : public std::runtime_error {
    EditorPreviewProjectionErrorCode code_;
    std::string field_;
    std::string adapter_;

  public:
    EditorPreviewProjectionError(EditorPreviewProjectionErrorCode code,
                                 std::string field, std::string adapter,
                                 std::string message);

    EditorPreviewProjectionErrorCode code() const noexcept { return code_; }
    const std::string &field() const noexcept { return field_; }
    const std::string &adapter() const noexcept { return adapter_; }
};

using EditorPreviewProjectionFaultHook =
    std::function<void(std::string_view phase, std::size_t index)>;

// Publish-ready data owned by one eval/render request.  This type deliberately
// has no publish, commit, merge, or rollback operation: destruction is the only
// terminal transition.
struct PreparedProjection {
    SceneProjectionState state;
    nlohmann::ordered_json evaluated_scene;
};

PreparedProjection prepareEditorPreviewProjection(
    const AuthoringSceneDocument &base_document,
    const nlohmann::json &overrides,
    const EditorPreviewProjectionFaultHook &fault_hook = {});
PreparedProjection prepareEditorPreviewProjection(
    const AuthoringSceneDocument &base_document,
    const nlohmann::json &overrides,
    const ResolvedSceneDefaults &defaults,
    const EditorPreviewProjectionFaultHook &fault_hook = {});
PreparedProjection prepareEditorPreviewProjection(
    const AuthoringSceneDocument &base_document,
    const nlohmann::json &overrides,
    const ResolvedSceneDefaults &defaults,
    PrefabRegistrySnapshot registry,
    BindableProviderSnapshot provider,
    const EditorPreviewProjectionFaultHook &fault_hook = {});

// Query adapter over an explicit PreparedProjection.  It never resolves a live
// ECS/Light/Phys/Camera module; physics receives the staged collider span.
class EditorPreviewEvaluationContext {
    const PreparedProjection &projection_;

  public:
    explicit EditorPreviewEvaluationContext(
        const PreparedProjection &projection) noexcept
        : projection_{projection} {}

    nlohmann::ordered_json evaluate(
        const nlohmann::json &queries,
        const EditorPreviewProjectionFaultHook &fault_hook = {}) const;
};

} // namespace Pelican
