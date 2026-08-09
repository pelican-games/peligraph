#include "gizmomodel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace PelicanStudio {
namespace {

using Json = nlohmann::json;

constexpr std::uint64_t MaximumExactJsonInteger = 9007199254740991ULL;
constexpr double GrabRadiusLogicalPixels = 10.0;
constexpr double DragThresholdLogicalPixels = 1.5;

enum class RequestKind : std::uint8_t {
    SetDisplay,
    QueryHandle,
};

struct Handle {
    GizmoMode mode = GizmoMode::Translate;
    GizmoAxis axis = GizmoAxis::X;
    std::array<double, 2> drag_direction{0.0, 0.0};
    double value_per_logical_pixel = 0.0;
};

std::optional<std::uint64_t> unsignedInteger(const Json &value) {
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value >= 0) {
            return static_cast<std::uint64_t>(signed_value);
        }
    }
    return std::nullopt;
}

std::optional<Handle> handleFromNames(std::string_view id,
                                      std::string_view axis) noexcept {
    const auto parsed_axis = [&]() -> std::optional<GizmoAxis> {
        if (axis == "x") return GizmoAxis::X;
        if (axis == "y") return GizmoAxis::Y;
        if (axis == "z") return GizmoAxis::Z;
        return std::nullopt;
    }();
    if (!parsed_axis) return std::nullopt;

    const std::string suffix =
        *parsed_axis == GizmoAxis::X
            ? "_x"
            : (*parsed_axis == GizmoAxis::Y ? "_y" : "_z");
    if (!id.ends_with(suffix)) return std::nullopt;
    if (id.starts_with("translate_")) {
        return Handle{GizmoMode::Translate, *parsed_axis};
    }
    if (id.starts_with("rotate_")) {
        return Handle{GizmoMode::Rotate, *parsed_axis};
    }
    if (id.starts_with("scale_")) {
        return Handle{GizmoMode::Scale, *parsed_axis};
    }
    return std::nullopt;
}

bool validVector(const Json &value, std::size_t size) {
    if (!value.is_array() || value.size() != size) return false;
    return std::all_of(value.begin(), value.end(), [](const Json &component) {
        return component.is_number() &&
               std::isfinite(component.get<double>());
    });
}

std::size_t axisIndex(GizmoAxis axis) noexcept {
    switch (axis) {
    case GizmoAxis::X: return 0;
    case GizmoAxis::Y: return 1;
    case GizmoAxis::Z: return 2;
    }
    return 0;
}

double pointerScalar(const Handle &handle, double dx, double dy) noexcept {
    return (dx * handle.drag_direction[0] +
            dy * handle.drag_direction[1]) *
           handle.value_per_logical_pixel;
}

Json translatedValue(const Json &baseline, GizmoAxis axis, double scalar) {
    Json result = baseline;
    const auto index = axisIndex(axis);
    result[index] = baseline[index].get<double>() + scalar;
    return result;
}

Json scaledValue(const Json &baseline, GizmoAxis axis, double scalar) {
    Json result = baseline;
    const auto index = axisIndex(axis);
    const double original = baseline[index].get<double>();
    const double exponent = std::clamp(scalar, -8.0, 8.0);
    result[index] =
        std::abs(original) > 1.0e-12 ? original * std::exp(exponent)
                                    : std::expm1(exponent);
    return result;
}

Json rotatedValue(const Json &baseline, GizmoAxis axis, double scalar) {
    const double half_angle = scalar * 0.5;
    const double sine = std::sin(half_angle);
    const double cosine = std::cos(half_angle);
    std::array<double, 4> delta{0.0, 0.0, 0.0, cosine};
    delta[axisIndex(axis)] = sine;

    const std::array<double, 4> base{
        baseline[0].get<double>(), baseline[1].get<double>(),
        baseline[2].get<double>(), baseline[3].get<double>()};
    // JSON quaternions use x/y/z/w. Pre-multiplication applies the selected
    // world-axis rotation for the unparented authoring transform, matching the
    // world-oriented rings rendered by the engine feature.
    std::array<double, 4> result{
        delta[3] * base[0] + delta[0] * base[3] +
            delta[1] * base[2] - delta[2] * base[1],
        delta[3] * base[1] - delta[0] * base[2] +
            delta[1] * base[3] + delta[2] * base[0],
        delta[3] * base[2] + delta[0] * base[1] -
            delta[1] * base[0] + delta[2] * base[3],
        delta[3] * base[3] - delta[0] * base[0] -
            delta[1] * base[1] - delta[2] * base[2],
    };
    const double norm = std::sqrt(
        result[0] * result[0] + result[1] * result[1] +
        result[2] * result[2] + result[3] * result[3]);
    if (!std::isfinite(norm) || norm <= std::numeric_limits<double>::epsilon()) {
        throw std::runtime_error("gizmo rotation produced an invalid quaternion");
    }
    for (double &component : result) component /= norm;
    return Json::array({result[0], result[1], result[2], result[3]});
}

const GizmoEditableField *fieldFor(const GizmoTransformBinding &binding,
                                   GizmoMode mode) noexcept {
    const std::optional<GizmoEditableField> *field = nullptr;
    switch (mode) {
    case GizmoMode::Translate: field = &binding.position; break;
    case GizmoMode::Rotate: field = &binding.rotation; break;
    case GizmoMode::Scale: field = &binding.scale; break;
    }
    return field->has_value() ? &**field : nullptr;
}

bool validField(const GizmoEditableField &field, GizmoMode mode) {
    return !field.field_key.empty() &&
           validVector(field.value, mode == GizmoMode::Rotate ? 4 : 3);
}

Json selectionJson(const OutlinerObjectKey &selection) {
    return Json{{"kind", "declaration"},
                {"scene_id", selection.scene_id},
                {"declaration_index", selection.declaration_index}};
}

bool resultSelectionMatches(const Json &value,
                            const OutlinerObjectKey &selection) {
    if (!value.is_object() || value.size() != 3) return false;
    const auto kind = value.find("kind");
    const auto scene = value.find("scene_id");
    const auto index = value.find("declaration_index");
    return kind != value.end() && kind->is_string() &&
           kind->get_ref<const std::string &>() == "declaration" &&
           scene != value.end() && scene->is_string() &&
           scene->get_ref<const std::string &>() == selection.scene_id &&
           index != value.end() && unsignedInteger(*index) ==
                                       selection.declaration_index;
}

} // namespace

std::string_view gizmoModeName(GizmoMode mode) noexcept {
    switch (mode) {
    case GizmoMode::Translate: return "translate";
    case GizmoMode::Rotate: return "rotate";
    case GizmoMode::Scale: return "scale";
    }
    return "translate";
}

struct GizmoModel::Impl {
    struct RequestState {
        RequestKind kind = RequestKind::SetDisplay;
        std::uint64_t generation = 0;
    };

    struct PendingHit {
        std::uint64_t request_id = 0;
        std::uint64_t gesture_id = 0;
        OutlinerObjectKey selection;
        GizmoMode mode = GizmoMode::Translate;
        GizmoPixelPosition pressed;
        GizmoPixelPosition current;
        std::optional<GizmoTransformBinding> binding;
        bool button_down = true;
    };

    enum class EditStage : std::uint8_t {
        BeginRequested,
        Editing,
    };

    struct ActiveGesture {
        std::uint64_t gesture_id = 0;
        Handle handle;
        GizmoPixelPosition pressed;
        GizmoPixelPosition current;
        GizmoEditableField field;
        Json baseline;
        Json last_value;
        double content_scale = 1.0;
        EditStage stage = EditStage::BeginRequested;
        bool button_down = true;
        bool changed = false;
    };

    bool connected = false;
    GizmoMode mode = GizmoMode::Translate;
    std::optional<OutlinerObjectKey> selection;
    std::uint64_t generation = 0;
    std::uint64_t next_request_id = 1;
    std::uint64_t next_gesture_id = 1;
    std::vector<GizmoRpcRequest> outgoing;
    std::unordered_map<std::uint64_t, RequestState> requests;
    std::optional<PendingHit> pending_hit;
    std::optional<ActiveGesture> active;
    std::vector<GizmoEditAction> edit_actions;
    std::vector<GizmoPixelPosition> fallback_picks;
    GizmoNotice notice;
    std::uint64_t notice_revision = 0;

    void setNotice(GizmoNoticeKind kind, std::string message) {
        GizmoNotice next{.kind = kind, .message = std::move(message)};
        if (notice.kind == next.kind && notice.message == next.message) return;
        notice = std::move(next);
        ++notice_revision;
    }

    std::uint64_t queue(std::string method, Json params,
                        RequestState state) {
        if (next_request_id == 0 ||
            next_request_id > MaximumExactJsonInteger) {
            throw std::overflow_error("gizmo RPC request identifier space exhausted");
        }
        const std::uint64_t id = next_request_id++;
        requests.emplace(id, state);
        outgoing.push_back(
            {.request_id = id, .method = std::move(method),
             .params = std::move(params)});
        return id;
    }

    void queueDisplay() {
        if (!connected) return;
        Json selected = nullptr;
        if (selection) selected = selectionJson(*selection);
        queue("set_gizmo",
              Json{{"selection", std::move(selected)},
                   {"mode", gizmoModeName(mode)}},
              {.kind = RequestKind::SetDisplay,
               .generation = generation});
    }

    void cancelInteraction() {
        ++generation;
        pending_hit.reset();
        if (active && active->stage == EditStage::Editing) {
            edit_actions.push_back(
                {.kind = GizmoEditActionKind::Finish,
                 .gesture_id = active->gesture_id,
                 .field_key = active->field.field_key,
                 .commit = false});
        }
        active.reset();
    }

    Json valueFor(const ActiveGesture &gesture) const {
        const double dx =
            static_cast<double>(gesture.current.x - gesture.pressed.x) /
            gesture.content_scale;
        const double dy =
            static_cast<double>(gesture.current.y - gesture.pressed.y) /
            gesture.content_scale;
        const double scalar = pointerScalar(gesture.handle, dx, dy);
        switch (gesture.handle.mode) {
        case GizmoMode::Translate:
            return translatedValue(gesture.baseline, gesture.handle.axis,
                                   scalar);
        case GizmoMode::Rotate:
            return rotatedValue(gesture.baseline, gesture.handle.axis,
                                scalar);
        case GizmoMode::Scale:
            return scaledValue(gesture.baseline, gesture.handle.axis,
                               scalar);
        }
        return gesture.baseline;
    }

    void previewCurrent() {
        if (!active || active->stage != EditStage::Editing) return;
        const double dx =
            static_cast<double>(active->current.x - active->pressed.x) /
            active->content_scale;
        const double dy =
            static_cast<double>(active->current.y - active->pressed.y) /
            active->content_scale;
        if (!active->changed &&
            std::hypot(dx, dy) < DragThresholdLogicalPixels) {
            return;
        }
        const Json value = valueFor(*active);
        if (value == active->last_value) return;
        active->last_value = value;
        active->changed = true;
        edit_actions.push_back(
            {.kind = GizmoEditActionKind::Preview,
             .gesture_id = active->gesture_id,
             .field_key = active->field.field_key,
             .value = value});
    }

    void finishActive() {
        if (!active || active->stage != EditStage::Editing) return;
        previewCurrent();
        edit_actions.push_back(
            {.kind = GizmoEditActionKind::Finish,
             .gesture_id = active->gesture_id,
             .field_key = active->field.field_key,
             .commit = active->changed});
        active.reset();
    }

    void failPendingHit(std::string message, bool fallback) {
        if (!pending_hit) return;
        if (fallback) fallback_picks.push_back(pending_hit->pressed);
        pending_hit.reset();
        if (!message.empty()) {
            setNotice(GizmoNoticeKind::Error, std::move(message));
        }
    }

    void handleQueryResult(std::uint64_t request_id, const Json &result) {
        if (!pending_hit || pending_hit->request_id != request_id) return;
        const PendingHit query = *pending_hit;
        const auto invalid = [&](std::string message) {
            failPendingHit("Invalid query_gizmo_handle response: " + message,
                           true);
        };

        if (!result.is_object()) {
            invalid("result must be an object");
            return;
        }
        const auto contract = result.find("contract");
        if (contract == result.end() || unsignedInteger(*contract) != 2) {
            invalid("contract 2 is required");
            return;
        }
        const auto result_selection = result.find("selection");
        if (result_selection == result.end() ||
            !resultSelectionMatches(*result_selection, query.selection)) {
            invalid("selection does not match the request");
            return;
        }
        const auto result_mode = result.find("mode");
        if (result_mode == result.end() || !result_mode->is_string() ||
            result_mode->get_ref<const std::string &>() !=
                gizmoModeName(query.mode)) {
            invalid("mode does not match the request");
            return;
        }
        const auto handle_value = result.find("handle");
        if (handle_value == result.end()) {
            invalid("handle is missing");
            return;
        }
        if (handle_value->is_null()) {
            fallback_picks.push_back(query.pressed);
            pending_hit.reset();
            setNotice(GizmoNoticeKind::None, {});
            return;
        }
        if (!handle_value->is_object()) {
            invalid("handle must be an object or null");
            return;
        }
        const auto id = handle_value->find("id");
        const auto axis = handle_value->find("axis");
        if (id == handle_value->end() || !id->is_string() ||
            axis == handle_value->end() || !axis->is_string()) {
            invalid("handle requires string id and axis");
            return;
        }
        auto handle = handleFromNames(
            id->get_ref<const std::string &>(),
            axis->get_ref<const std::string &>());
        if (!handle || handle->mode != query.mode) {
            invalid("handle is inconsistent with the requested mode");
            return;
        }
        const auto direction = handle_value->find("drag_direction");
        if (direction == handle_value->end() || !direction->is_object() ||
            direction->size() != 2 || !direction->contains("x") ||
            !direction->contains("y") ||
            !direction->at("x").is_number() ||
            !direction->at("y").is_number()) {
            invalid("handle drag_direction requires exactly numeric x and y");
            return;
        }
        const double direction_x = direction->at("x").get<double>();
        const double direction_y = direction->at("y").get<double>();
        const double direction_length = std::hypot(direction_x, direction_y);
        if (!std::isfinite(direction_length) ||
            std::abs(direction_length - 1.0) > 1.0e-4) {
            invalid("handle drag_direction must be a finite unit vector");
            return;
        }
        const auto value_per_pixel =
            handle_value->find("value_per_logical_pixel");
        if (value_per_pixel == handle_value->end() ||
            !value_per_pixel->is_number() ||
            !std::isfinite(value_per_pixel->get<double>()) ||
            value_per_pixel->get<double>() <= 0.0) {
            invalid("handle value_per_logical_pixel must be positive and finite");
            return;
        }
        handle->drag_direction = {direction_x, direction_y};
        handle->value_per_logical_pixel = value_per_pixel->get<double>();
        const auto grab_radius = result.find("grab_radius_pixels");
        if (grab_radius == result.end() || !grab_radius->is_number() ||
            !std::isfinite(grab_radius->get<double>()) ||
            grab_radius->get<double>() <= 0.0) {
            invalid("grab_radius_pixels must be positive and finite");
            return;
        }
        if (!query.binding || query.binding->selection != query.selection) {
            failPendingHit(
                "The Inspector transform is not ready for gizmo editing.",
                false);
            return;
        }
        const GizmoEditableField *field = fieldFor(*query.binding, query.mode);
        if (field == nullptr || !validField(*field, query.mode)) {
            failPendingHit(
                "The selected transform field is not authored or editable.",
                false);
            return;
        }

        const double content_scale = std::clamp(
            grab_radius->get<double>() / GrabRadiusLogicalPixels, 0.5, 4.0);
        active = ActiveGesture{
            .gesture_id = query.gesture_id,
            .handle = *handle,
            .pressed = query.pressed,
            .current = query.current,
            .field = *field,
            .baseline = field->value,
            .last_value = field->value,
            .content_scale = content_scale,
            .stage = EditStage::BeginRequested,
            .button_down = query.button_down,
        };
        pending_hit.reset();
        edit_actions.push_back(
            {.kind = GizmoEditActionKind::Begin,
             .gesture_id = active->gesture_id,
             .field_key = active->field.field_key});
        setNotice(GizmoNoticeKind::None, {});
    }

    bool acceptsResponse(std::uint64_t request_id,
                         const RequestState &state) const noexcept {
        if (state.generation != generation) return false;
        return state.kind != RequestKind::QueryHandle ||
               (pending_hit && pending_hit->request_id == request_id);
    }
};

GizmoModel::GizmoModel() : impl_{std::make_unique<Impl>()} {}

GizmoModel::~GizmoModel() = default;

void GizmoModel::startSession() {
    impl_->cancelInteraction();
    impl_->connected = true;
    impl_->requests.clear();
    impl_->outgoing.clear();
    impl_->queueDisplay();
}

void GizmoModel::stopSession() {
    impl_->cancelInteraction();
    impl_->connected = false;
    impl_->requests.clear();
    impl_->outgoing.clear();
}

void GizmoModel::setSelection(
    std::optional<OutlinerObjectKey> selection) {
    if (impl_->selection == selection) return;
    impl_->cancelInteraction();
    impl_->selection = std::move(selection);
    impl_->queueDisplay();
}

void GizmoModel::setMode(GizmoMode mode) {
    if (impl_->mode == mode) return;
    impl_->cancelInteraction();
    impl_->mode = mode;
    impl_->queueDisplay();
}

void GizmoModel::pointerPressed(
    GizmoPixelPosition position,
    std::optional<GizmoTransformBinding> transform_binding) {
    if (position.x < 0 || position.y < 0) return;
    impl_->cancelInteraction();
    if (!impl_->selection || !impl_->connected) {
        impl_->fallback_picks.push_back(position);
        return;
    }
    if (impl_->next_gesture_id == 0 ||
        impl_->next_gesture_id > MaximumExactJsonInteger) {
        throw std::overflow_error("gizmo gesture identifier space exhausted");
    }
    const std::uint64_t gesture_id = impl_->next_gesture_id++;
    const std::uint64_t request_id = impl_->queue(
        "query_gizmo_handle",
        Json{{"selection", selectionJson(*impl_->selection)},
             {"mode", gizmoModeName(impl_->mode)},
             {"x", position.x},
             {"y", position.y}},
        {.kind = RequestKind::QueryHandle,
         .generation = impl_->generation});
    impl_->pending_hit = Impl::PendingHit{
        .request_id = request_id,
        .gesture_id = gesture_id,
        .selection = *impl_->selection,
        .mode = impl_->mode,
        .pressed = position,
        .current = position,
        .binding = std::move(transform_binding),
    };
}

void GizmoModel::pointerMoved(GizmoPixelPosition position) {
    if (impl_->pending_hit) {
        impl_->pending_hit->current = position;
    }
    if (impl_->active) {
        impl_->active->current = position;
        impl_->previewCurrent();
    }
}

void GizmoModel::pointerReleased(GizmoPixelPosition position) {
    if (impl_->pending_hit) {
        impl_->pending_hit->current = position;
        impl_->pending_hit->button_down = false;
    }
    if (!impl_->active) return;
    impl_->active->current = position;
    impl_->active->button_down = false;
    if (impl_->active->stage == Impl::EditStage::Editing) {
        impl_->finishActive();
    }
}

std::vector<GizmoRpcRequest> GizmoModel::takeRpcRequests() {
    return std::exchange(impl_->outgoing, {});
}

void GizmoModel::receiveRpcResult(std::uint64_t request_id,
                                  std::string_view result_json) {
    const auto found = impl_->requests.find(request_id);
    if (found == impl_->requests.end()) return;
    const Impl::RequestState state = found->second;
    impl_->requests.erase(found);
    if (!impl_->acceptsResponse(request_id, state)) return;

    Json result;
    try {
        result = Json::parse(result_json);
    } catch (const Json::exception &error) {
        if (state.kind == RequestKind::QueryHandle) {
            impl_->failPendingHit(
                "query_gizmo_handle returned invalid JSON: " +
                    std::string{error.what()},
                true);
        } else {
            impl_->setNotice(
                GizmoNoticeKind::Error,
                "set_gizmo returned invalid JSON: " +
                    std::string{error.what()});
        }
        return;
    }

    if (state.kind == RequestKind::SetDisplay) {
        if (!result.is_object() ||
            unsignedInteger(result.value("contract", Json{})) != 1) {
            impl_->setNotice(GizmoNoticeKind::Error,
                             "set_gizmo returned an invalid response.");
            return;
        }
        impl_->setNotice(GizmoNoticeKind::None, {});
        return;
    }
    impl_->handleQueryResult(request_id, result);
}

void GizmoModel::receiveRpcFailure(std::uint64_t request_id,
                                   std::string message) {
    const auto found = impl_->requests.find(request_id);
    if (found == impl_->requests.end()) return;
    const Impl::RequestState state = found->second;
    impl_->requests.erase(found);
    if (!impl_->acceptsResponse(request_id, state)) return;
    if (message.empty()) message = "RPC failed without an error message";
    if (state.kind == RequestKind::QueryHandle) {
        impl_->failPendingHit("Gizmo handle query failed: " + message, true);
        return;
    }
    impl_->setNotice(GizmoNoticeKind::Error,
                     "Gizmo display failed: " + message);
}

std::vector<GizmoEditAction> GizmoModel::takeEditActions() {
    return std::exchange(impl_->edit_actions, {});
}

std::vector<GizmoPixelPosition> GizmoModel::takeFallbackPicks() {
    return std::exchange(impl_->fallback_picks, {});
}

void GizmoModel::confirmEditStarted(std::uint64_t gesture_id, bool accepted,
                                    std::string message) {
    if (!impl_->active || impl_->active->gesture_id != gesture_id ||
        impl_->active->stage != Impl::EditStage::BeginRequested) {
        return;
    }
    if (!accepted) {
        impl_->active.reset();
        if (message.empty()) {
            message = "Finish the active Inspector edit before using the gizmo.";
        }
        impl_->setNotice(GizmoNoticeKind::Error, std::move(message));
        return;
    }
    impl_->active->stage = Impl::EditStage::Editing;
    impl_->previewCurrent();
    if (impl_->active && !impl_->active->button_down) {
        impl_->finishActive();
    }
}

void GizmoModel::cancelActiveEdit(std::string message) {
    impl_->cancelInteraction();
    if (!message.empty()) {
        impl_->setNotice(GizmoNoticeKind::Error, std::move(message));
    }
}

GizmoMode GizmoModel::mode() const noexcept { return impl_->mode; }

const std::optional<OutlinerObjectKey> &GizmoModel::selection() const noexcept {
    return impl_->selection;
}

const GizmoNotice &GizmoModel::notice() const noexcept {
    return impl_->notice;
}

std::uint64_t GizmoModel::noticeRevision() const noexcept {
    return impl_->notice_revision;
}

bool GizmoModel::gestureActive() const noexcept {
    return impl_->pending_hit.has_value() || impl_->active.has_value();
}

} // namespace PelicanStudio
