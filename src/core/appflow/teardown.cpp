#include "teardown.hpp"

#include "../ecs/core.hpp"
#include "../log.hpp"
#include "../phys/physworld.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../vkcore/core.hpp"

namespace Pelican {
namespace {

template <class Function>
void cleanupStep(const char *name, Function &&function) noexcept {
    try {
        function();
    } catch (const std::exception &error) {
        if (logger != nullptr) {
            LOG_ERROR(logger, "runtime teardown step '{}' failed: {}", name, error.what());
        }
    } catch (...) {
        if (logger != nullptr) {
            LOG_ERROR(logger, "runtime teardown step '{}' failed with a non-standard exception", name);
        }
    }
}

} // namespace

void teardownRuntimeNoThrow() noexcept {
    if (VulkanManageCore::__get().has_value()) {
        cleanupStep("wait-idle", [] { VulkanManageCore::__get()->waitIdle(); });
    }
    if (PhysWorld::__get().has_value()) {
        cleanupStep("physics", [] { PhysWorld::__get()->clear(); });
    }
    if (ECSCore::__get().has_value()) {
        cleanupStep("ecs", [] { ECSCore::__get()->clearEntities(); });
    }
    if (PolygonInstanceContainer::__get().has_value()) {
        cleanupStep("model-instances", [] { PolygonInstanceContainer::__get()->clear(); });
    }
}

RuntimeTeardownGuard::~RuntimeTeardownGuard() {
    run();
}

void RuntimeTeardownGuard::run() noexcept {
    if (completed) {
        return;
    }
    completed = true;
    teardownRuntimeNoThrow();
}

} // namespace Pelican
