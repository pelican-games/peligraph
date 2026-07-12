#include "imguiruntime.hpp"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {

bool isImGuiRuntimeEnabled(const EngineLaunchConfig &config) noexcept {
    return !config.headless && !config.rpc && !config.input_replay && !config.golden_mode;
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
        const auto anchor = std::find_if(passes.begin(), passes.end(), [](const auto &pass) {
            return pass.is_object() &&
                   pass.value("type", std::string{}) == "canonical_anchor" &&
                   pass.value("anchor", std::string{}) == "imgui";
        });
        if (anchor == passes.end()) {
            throw std::runtime_error("Canonical imgui anchor is missing from composed frame graph");
        }

        nlohmann::json imgui_pass{
            {"name", "imgui_pass"},
            {"type", "imgui"},
            {"after", nlohmann::json::array({anchor->at("name")})},
            {"output", {{"color", "display"}, {"depth", nullptr}}},
            {"color_load_op", "Load"},
            {"color_store_op", "Store"},
        };
        passes.insert(std::next(anchor), std::move(imgui_pass));
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
