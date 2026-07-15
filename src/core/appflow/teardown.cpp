#include "teardown.hpp"

#include "../ecs/core.hpp"
#include "../log.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
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
    if (auto *vulkan = FastModuleContainer::tryGet<VulkanManageCore>())
        cleanupStep("wait-idle", [vulkan] { vulkan->waitIdle(); });
#if PELICAN_WITH_PHYSICS
    if (auto *physics = FastModuleContainer::tryGet<PhysWorld>())
        cleanupStep("physics", [physics] { physics->clear(); });
#endif
    if (auto *ecs = FastModuleContainer::tryGet<ECSCore>())
        cleanupStep("ecs", [ecs] { ecs->clearEntities(); });
    if (auto *instances = FastModuleContainer::tryGet<PolygonInstanceContainer>())
        cleanupStep("model-instances", [instances] { instances->clear(); });
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
