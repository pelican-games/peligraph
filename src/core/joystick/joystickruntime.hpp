#pragma once

#include "../container.hpp"
#include "joystickmonitor.hpp"

#include <memory>

namespace Pelican {
std::unique_ptr<JoystickSource> makeGlfwJoystickSource();

class JoystickRuntime final : public ModuleBase<JoystickRuntime> {
  public:
    JoystickRuntime();
    void pumpFrame();
    nlohmann::json devicesJson(bool with_values, const std::string &engine_instance_id);
    nlohmann::json injectJson(const nlohmann::json &params);

  private:
    std::unique_ptr<JoystickSource> source_;
    InjectedJoystickSource *injected_ = nullptr;
    std::optional<JoystickMonitor> monitor_;
};
} // namespace Pelican
