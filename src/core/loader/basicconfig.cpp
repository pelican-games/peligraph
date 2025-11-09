#include "basicconfig.hpp"
#include "../log.hpp"

namespace Pelican {

ProjectBasicConfig::ProjectBasicConfig(DependencyContainer &con) { LOG_INFO(logger, "project basic config loaded"); }

std::string ProjectBasicConfig::windowTitle() const { return "Pelican App"; }
ProjectBasicConfig::window_size ProjectBasicConfig::initialWindowSize() const {
    return window_size{
        .width = 1280,
        .height = 720,
    };
}
bool ProjectBasicConfig::initialFullScreenState() const { return false; }

float ProjectBasicConfig::framerateTarget() const { return 60.0f; }

} // namespace Pelican
