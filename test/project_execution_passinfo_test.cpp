#include "../src/project/passinfo.hpp"

bool projectPassInfoDefaults() {
    using namespace Pelican;
    const FullscreenPassInfo fullscreen;
    const GenericRasterPassInfo raster;
    const DebugDrawPassInfo debug_draw;
    const GizmoPassInfo gizmo;
    const DebugTextPassInfo debug_text;
    const ShadowDepthPassInfo shadow;
    const VelocityPassInfo velocity;
    const PickingPassInfo picking;
    [[maybe_unused]] const UiPassInfo ui;
    return !fullscreen.raster_state && !raster.frag_shader &&
           fullscreen.vert_shader.stage == ShaderStage::vertex &&
           debug_draw.frag_shader.stage == ShaderStage::fragment &&
           gizmo.frag_shader.stage == ShaderStage::fragment &&
           debug_text.frag_shader.stage == ShaderStage::fragment &&
           shadow.vert_shader.stage == ShaderStage::vertex &&
           velocity.skinned_vert_shader.stage == ShaderStage::vertex &&
           picking.frag_shader.stage == ShaderStage::fragment;
}
