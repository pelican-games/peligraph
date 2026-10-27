#include "imguiruntime.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {

bool isImGuiRuntimeEnabled(const EngineLaunchConfig &config) noexcept {
    // XR v1 deliberately has no engine ImGui UI. The XR graph has no ImGui
    // pass, so beginning an ImGui frame while XR is active would leave that
    // frame without a matching Render/EndFrame call.
    return !config.headless && !config.rpc && !config.input_replay && !config.golden_mode &&
           !config.xr_active;
}

bool invokeImGuiRuntimeCallback(const EngineLaunchConfig &config,
                                const std::function<void()> &callback) {
    if (!isImGuiRuntimeEnabled(config)) {
        return false;
    }
    callback();
    return true;
}

void appendImGuiPassToCanonicalGraphs(nlohmann::json &composed_config) {
    if (!composed_config.contains("rendering_passes") ||
        !composed_config.at("rendering_passes").is_array()) {
        throw std::runtime_error("ImGui requires rendering_passes in the composed config");
    }

    for (auto &rendering_pass : composed_config.at("rendering_passes")) {
        auto &passes = rendering_pass.at("passes");
        const auto terminal = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
            return pass.is_object() && pass.value("type", std::string{}) == "output_transform";
        });
        if (terminal == passes.end()) {
            throw std::runtime_error("ImGui requires output_transform in the composed frame graph");
        }
        auto predecessors = nlohmann::json::array();
        for (const auto &pass : passes)
            if (pass.value("type", std::string{}) != "output_transform")
                predecessors.push_back(pass.at("name"));
        nlohmann::json imgui_pass{
            {"name", "imgui_pass"},
            {"type", "imgui"},
            {"after", std::move(predecessors)},
            // RC-3b: written in the normal form (renderingjsonnormalize.hpp).
            {"output", {{"color", nlohmann::json::array({nlohmann::json{{"target", "display"}}})}, {"depth", nullptr}}},
            {"color_load_op", "load"},
            {"color_store_op", "store"},
        };
        (*terminal)["after"].push_back("imgui_pass");
        passes.insert(terminal, std::move(imgui_pass));
    }
}

namespace {

template <class Input>
void applyCapture(Input &input, bool want_keyboard, bool want_pointer) noexcept {
    if (want_keyboard) {
        for (auto value = static_cast<std::underlying_type_t<KeyCode>>(KeyCode::A);
             value <= static_cast<std::underlying_type_t<KeyCode>>(KeyCode::F12); ++value) {
            input.consumeControlForActions(static_cast<KeyCode>(value));
        }
    }
    if (want_pointer) {
        input.consumePointerForActions();
    }
}

} // namespace

void applyImGuiCaptureForActions(InputStateCore &input, bool want_keyboard,
                                 bool want_pointer) noexcept {
    applyCapture(input, want_keyboard, want_pointer);
}

void applyImGuiCaptureForActions(InputState &input, bool want_keyboard,
                                 bool want_pointer) noexcept {
    applyCapture(input, want_keyboard, want_pointer);
}

} // namespace Pelican
