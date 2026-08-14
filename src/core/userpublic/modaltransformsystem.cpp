#include "gamesystem.hpp"

#include "../ecs/core.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/scene.hpp"
#include "../os/actionmap.hpp"
#include "../os/inputstate.hpp"
#include "../os/window.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/gizmo.hpp"
#include "../renderer/modaltransform.hpp"
#include "../vkcore/rendertarget.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

bool modalActionsConfigured() {
    const auto *map = internal::inputActionMap();
    if (map == nullptr ||
        map->findActionSet(modalTransformActionSet) == nullptr) {
        return false;
    }

    constexpr std::array actions{
        modalTranslateAction, modalRotateAction, modalScaleAction,
        modalAxisXAction, modalAxisYAction, modalAxisZAction,
        modalConfirmAction, modalCancelAction,
    };
    for (const auto action_name : actions) {
        const auto *action = map->findAction(action_name);
        if (action == nullptr || action->set_name != modalTransformActionSet) {
            throw std::runtime_error(
                "input action set '" + std::string{modalTransformActionSet} +
                "' is missing action '" + std::string{action_name} + "'");
        }
        if (action->type != InputActionType::button) {
            throw std::runtime_error(
                "modal transform action '" + std::string{action_name} +
                "' must be a button action");
        }
    }
    return true;
}

std::optional<GizmoMode> pressedMode(GameContext &ctx) {
    if (ctx.actionPressed(modalTranslateAction)) {
        return GizmoMode::translate;
    }
    if (ctx.actionPressed(modalRotateAction)) {
        return GizmoMode::rotate;
    }
    if (ctx.actionPressed(modalScaleAction)) {
        return GizmoMode::scale;
    }
    return std::nullopt;
}

std::optional<GizmoAxis> pressedAxis(GameContext &ctx) {
    if (ctx.actionPressed(modalAxisXAction)) return GizmoAxis::x;
    if (ctx.actionPressed(modalAxisYAction)) return GizmoAxis::y;
    if (ctx.actionPressed(modalAxisZAction)) return GizmoAxis::z;
    return std::nullopt;
}

float currentContentScale(vk::Extent2D extent) noexcept {
    const auto *window = FastModuleContainer::tryGet<Window>();
    return window != nullptr
               ? gizmoContentScale(extent, window->logicalExtent())
               : 1.0f;
}

class BuiltinModalTransformSystem {
  public:
    void update(GameContext &ctx) {
        auto &state = GET_MODULE(ModalTransformState);
        ModalTransformFrameInput input;
        input.enabled = modalActionsConfigured();
        if (!input.enabled) {
            state.update(input);
            return;
        }

        input.mode_pressed = pressedMode(ctx);
        input.axis_pressed = pressedAxis(ctx);
        input.confirm_pressed = ctx.actionPressed(modalConfirmAction);
        input.cancel_pressed = ctx.actionPressed(modalCancelAction);
        const auto &snapshot = GET_MODULE(InputState).currentSnapshot();
        input.pointer_delta_logical =
            {snapshot.mouse_delta_x, snapshot.mouse_delta_y};

        auto *gizmo = FastModuleContainer::tryGet<Gizmo>();
        const auto display = gizmo != nullptr
                                 ? gizmo->displayRequest()
                                 : std::optional<GizmoDisplayRequest>{};
        std::optional<GizmoTargetTransform> target;
        if (display) {
            target = resolveGizmoTargetTransform(
                display->selection, GET_MODULE(ProjectBasicConfig),
                GET_MODULE(SceneLoader), GET_MODULE(ECSCore));
            if (target) input.selection = display->selection;
        }

        const auto before = state.snapshot();
        const auto effective_mode =
            before.phase == ModalTransformPhase::active && before.mode
                ? before.mode
                : input.mode_pressed;
        const auto effective_axis = input.axis_pressed;
        if (target && effective_mode) {
            auto &camera = GET_MODULE(Camera);
            const auto extent = GET_MODULE(RenderTarget).getExtent();
            const auto content_scale = currentContentScale(extent);
            if (effective_axis) {
                const auto geometry = buildGizmoGeometry(
                    *effective_mode, target->position,
                    camera.getVPMatrix(), extent, content_scale);
                input.axis_projection = gizmoDragProjectionForAxis(
                    geometry, *effective_mode, *effective_axis);
            }
            if (*effective_mode == GizmoMode::translate &&
                !(before.phase == ModalTransformPhase::active &&
                  before.axis) && !effective_axis) {
                input.view_plane_projection =
                    buildGizmoViewPlaneDragProjection(
                        target->position, camera.getVPMatrix(),
                        camera.getDir(), camera.getUp(), extent,
                        content_scale);
            }
            input.view_rotation_axis = camera.getDir();
        }

        state.update(input);
        const auto after = state.snapshot();
        if (gizmo != nullptr && display && after.selection && after.mode &&
            display->selection == *after.selection &&
            display->mode != *after.mode) {
            gizmo->setDisplayRequest(GizmoDisplayRequest{
                .selection = display->selection,
                .mode = *after.mode,
            });
        }
    }
};

} // namespace

// Camera controllers run at 10000. Modal view-plane motion intentionally sees
// their current-frame pose so middle-button orbit remains active underneath
// the editor action set.
PELICAN_REGISTER_SYSTEM(BuiltinModalTransformSystem, 10010);

} // namespace Pelican
