#pragma once

#include "fullscreeninputbindingmismatch.hpp"

#include <nlohmann/json.hpp>
#include <span>

namespace Pelican {

struct FullscreenReloadRefusal;

// A shader's pass-input declarations as Studio reads them wherever they are
// told (get_frame_plan's pass_inputs.declarations, a refusal's
// shader_pass_inputs): [{binding, type, name, variadic}], binding order.
nlohmann::ordered_json passInputDeclarationsJson(
    std::span<const FullscreenShaderPassInput> declarations);

// Q-14e / Q-14f / VI-1: the shader_input_mismatch field of a refusal. The
// editor's render_pipeline_preflight_failed refusals, a reload's refused
// passes and get_frame_plan's pass_inputs.reload_refused all carry this one
// shape.
nlohmann::ordered_json shaderInputMismatchJson(
    const FullscreenInputBindingMismatchInfo &info);

// VI-1b: a refused shader reload, as get_frame_plan's
// pass_inputs.reload_refused tells it: {reason, shader_input_mismatch,
// declarations} (declarations: the refused shader's). With `named`, the
// pass and fragment lead it, as reload_shaders' refused_passes and the
// reload status tell it.
nlohmann::ordered_json reloadRefusalJson(const FullscreenReloadRefusal &refusal,
                                         bool named);

} // namespace Pelican
