#pragma once

#include "../launchconfig.hpp"
#include "../os/inputstate.hpp"

#include <functional>
#include <nlohmann/json_fwd.hpp>

namespace Pelican {

bool isImGuiRuntimeEnabled(const EngineLaunchConfig &config) noexcept;

// All engine calls into the tool unit cross this gate. Deterministic drivers
// therefore skip callbacks, instead of running an invisible ImGui frame.
bool invokeImGuiRuntimeCallback(const EngineLaunchConfig &config,
                                const std::function<void()> &callback);

void appendImGuiPassToCanonicalGraphs(nlohmann::json &composed_config);

// Applies ImGui's coarse frame capture before pelican.ui and gameplay freeze.
void applyImGuiCaptureForActions(InputStateCore &input, bool want_keyboard,
                                 bool want_pointer) noexcept;
void applyImGuiCaptureForActions(InputState &input, bool want_keyboard,
                                 bool want_pointer) noexcept;

} // namespace Pelican
