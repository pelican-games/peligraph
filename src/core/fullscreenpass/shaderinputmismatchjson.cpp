#include "shaderinputmismatchjson.hpp"

#include "fullscreenpasscontainer.hpp"

#include <utility>

namespace Pelican {

nlohmann::ordered_json passInputDeclarationsJson(
    std::span<const FullscreenShaderPassInput> declarations) {
    auto result = nlohmann::ordered_json::array();
    for (const auto &input : declarations) {
        result.push_back(nlohmann::ordered_json{{"binding", input.binding},
                                                {"type", input.type},
                                                {"name", input.name},
                                                {"variadic", input.variadic}});
    }
    return result;
}

nlohmann::ordered_json shaderInputMismatchJson(
    const FullscreenInputBindingMismatchInfo &info) {
    nlohmann::ordered_json field{{"pass", info.pass},
                                 {"fragment", info.fragment},
                                 // Q-14f: which way round they disagree.
                                 {"direction", info.direction},
                                 {"kind", info.kind},
                                 {"binding", info.binding}};
    if (!info.resource.empty()) field["resource"] = info.resource;
    field["expected"] = info.expected;
    field["pass_texture_inputs"] = info.pass_texture_inputs;
    field["pass_buffer_inputs"] = info.pass_buffer_inputs;
    field["shader_pass_inputs"] =
        passInputDeclarationsJson(info.shader_pass_inputs);
    // VI-1: the shader's variadic group and, for a layout refusal, the rule
    // it breaks.
    field["variadic_range"] =
        info.variadic_range
            ? nlohmann::ordered_json{{"first", info.variadic_range->first},
                                     {"max", info.variadic_range->max}}
            : nlohmann::ordered_json(nullptr);
    if (!info.variadic_rule.empty()) {
        field["variadic_rule"] = info.variadic_rule;
    }
    return field;
}

nlohmann::ordered_json reloadRefusalJson(const FullscreenReloadRefusal &refusal,
                                         bool named) {
    nlohmann::ordered_json result = nlohmann::ordered_json::object();
    if (named) {
        result["pass"] = refusal.mismatch.pass;
        result["fragment"] = refusal.mismatch.fragment;
    }
    result["reason"] = refusal.reason;
    result["shader_input_mismatch"] = shaderInputMismatchJson(refusal.mismatch);
    result["declarations"] =
        passInputDeclarationsJson(refusal.mismatch.shader_pass_inputs);
    return result;
}

} // namespace Pelican
