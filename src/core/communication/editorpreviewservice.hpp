#pragma once

#include "../loader/editorpreviewprojection.hpp"
#include "../vkcore/previewexecutor.hpp"

#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {

enum class EditorPreviewErrorCode : std::uint8_t {
    schema_violation,
    method_unavailable,
    gate_closed,
    xr_active_unsupported,
    capture_schema_violation,
    capture_too_large,
    state_changed,
};

std::string_view editorPreviewErrorCodeName(EditorPreviewErrorCode code) noexcept;

class EditorPreviewError : public std::runtime_error {
    EditorPreviewErrorCode code_;
    nlohmann::ordered_json payload_;

  public:
    EditorPreviewError(EditorPreviewErrorCode code, std::string message,
                       nlohmann::ordered_json payload = nlohmann::ordered_json::object());
    EditorPreviewErrorCode code() const noexcept { return code_; }
    const nlohmann::ordered_json &payload() const noexcept { return payload_; }
};

struct EditorPreviewGateSnapshot {
    bool can_preview = false;
    std::uint64_t epoch = 0;
    std::vector<std::string> reasons;
};

using EditorPreviewGateProvider = std::function<EditorPreviewGateSnapshot()>;

struct EditorPreviewServiceDependencies {
    std::function<const AuthoringSceneDocument &()> document;
    std::function<const SceneProjectionState &()> projection_state;
    std::function<const PreviewGraphProgram &()> preview_graph;
    std::function<bool()> xr_active;
    std::function<PreviewEngineTimeSnapshot()> engine_time;
    // A canonical exact snapshot of every shared category in the isolation
    // contract.  Tests supply sentinels; production supplies live diagnostics.
    std::function<nlohmann::ordered_json()> shared_state_snapshot;
    EditorPreviewProjectionFaultHook projection_fault_hook;
    std::function<void(std::string_view)> execution_fault_hook;
};

class EditorPreviewService {
    EditorPreviewServiceDependencies dependencies_;
    PreviewExecutor executor_;

  public:
    explicit EditorPreviewService(EditorPreviewServiceDependencies dependencies);

    nlohmann::ordered_json evalPreview(
        const nlohmann::json &params,
        const EditorPreviewGateProvider &gate_provider) const;
    nlohmann::ordered_json renderPreview(
        const nlohmann::json &params,
        const EditorPreviewGateProvider &gate_provider) const;
};

} // namespace Pelican
