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

// How often Studio has to ask the engine for the modal transform state.
//
// A modal transform is begun by a key pressed inside the engine's own window,
// so the engine cannot tell Studio about it -- Studio has to ask. But it only
// has to ask while the answer can change: while a gesture is in flight, and
// while the engine viewport is where the user's input is going and such a key
// could land. With the engine window neither hovered nor focused and no
// gesture running, nobody can begin a modal transform, and the question has
// nothing to ask about.
inline constexpr int GizmoModalActivePollIntervalMs = 16;
inline constexpr int GizmoModalWatchPollIntervalMs = 100;

// Milliseconds between `get_modal_transform` calls; 0 means "do not ask at
// all".
int gizmoModalPollIntervalMs(bool rpc_connected, bool gesture_active,
                             bool viewport_input_active) noexcept;

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

// Qt-independent controller for the public gizmo RPC surface. Handle dragging
// remains local interaction state; modal transform mode/axis/delta state is
// polled from the engine and is adapted onto the same authoring preview lease.
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
    void pollModalTransform(
        std::optional<GizmoTransformBinding> transform_binding);

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
    std::string_view bindingDisplay(GizmoMode mode) const noexcept;
    std::uint64_t bindingRevision() const noexcept;
    bool gestureActive() const noexcept;
};

} // namespace PelicanStudio
