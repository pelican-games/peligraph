#pragma once

#include "project.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace PelicanStudio {

enum class GizmoMode : std::uint8_t {
    Translate,
    Rotate,
    Scale,
};

enum class GizmoAxis : std::uint8_t {
    X,
    Y,
    Z,
};

std::string_view gizmoModeName(GizmoMode mode) noexcept;

struct GizmoPixelPosition {
    int x = 0;
    int y = 0;

    bool operator==(const GizmoPixelPosition &) const = default;
};

struct GizmoEditableField {
    std::string field_key;
    nlohmann::json value;
};

// Captured from the Inspector's current authoring snapshot. Missing fields are
// kept missing: a schema default that was never authored must not silently turn
// into a different authoring value just because its handle was clicked.
struct GizmoTransformBinding {
    OutlinerObjectKey selection;
    std::optional<GizmoEditableField> position;
    std::optional<GizmoEditableField> rotation;
    std::optional<GizmoEditableField> scale;
};

struct GizmoRpcRequest {
    std::uint64_t request_id = 0;
    std::string method;
    nlohmann::json params;
};

enum class GizmoEditActionKind : std::uint8_t {
    Begin,
    Preview,
    Finish,
};

struct GizmoEditAction {
    GizmoEditActionKind kind = GizmoEditActionKind::Begin;
    std::uint64_t gesture_id = 0;
    std::string field_key;
    nlohmann::json value;
    bool commit = false;
};

enum class GizmoNoticeKind : std::uint8_t {
    None,
    Error,
};

struct GizmoNotice {
    GizmoNoticeKind kind = GizmoNoticeKind::None;
    std::string message;
};

// Qt-independent controller for the public gizmo RPC surface. The model owns
// only editor interaction state. The engine remains stateless between the one
// hit query on pointer press and the eventual authoring preview operations.
class GizmoModel {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    GizmoModel();
    ~GizmoModel();

    GizmoModel(const GizmoModel &) = delete;
    GizmoModel &operator=(const GizmoModel &) = delete;

    void startSession();
    void stopSession();
    void setSelection(std::optional<OutlinerObjectKey> selection);
    void setMode(GizmoMode mode);

    void pointerPressed(
        GizmoPixelPosition position,
        std::optional<GizmoTransformBinding> transform_binding);
    void pointerMoved(GizmoPixelPosition position);
    void pointerReleased(GizmoPixelPosition position);

    std::vector<GizmoRpcRequest> takeRpcRequests();
    void receiveRpcResult(std::uint64_t request_id,
                          std::string_view result_json);
    void receiveRpcFailure(std::uint64_t request_id, std::string message);

    std::vector<GizmoEditAction> takeEditActions();
    std::vector<GizmoPixelPosition> takeFallbackPicks();
    void confirmEditStarted(std::uint64_t gesture_id, bool accepted,
                            std::string message = {});
    void cancelActiveEdit(std::string message = {});

    GizmoMode mode() const noexcept;
    const std::optional<OutlinerObjectKey> &selection() const noexcept;
    const GizmoNotice &notice() const noexcept;
    std::uint64_t noticeRevision() const noexcept;
    bool gestureActive() const noexcept;
};

} // namespace PelicanStudio
