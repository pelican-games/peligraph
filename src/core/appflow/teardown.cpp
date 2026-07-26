#include "teardown.hpp"

#include "../ecs/core.hpp"
#include "../gamelogic/behaviorarena.hpp"
#include "../log.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physworld.hpp"
#endif
#include "../renderer/polygoninstancecontainer.hpp"
#include "../vkcore/core.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "../userpublic/details/event/registerer.hpp"

#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

template <class Function>
bool cleanupStep(const char *name, Function &&function) noexcept {
    try {
        function();
        return true;
    } catch (const std::exception &error) {
        try {
            if (logger != nullptr) {
                LOG_ERROR(logger, "runtime teardown step '{}' failed: {}", name,
                          error.what());
            }
        } catch (...) {
        }
    } catch (...) {
        try {
            if (logger != nullptr) {
                LOG_ERROR(
                    logger,
                    "runtime teardown step '{}' failed with a non-standard exception",
                    name);
            }
        } catch (...) {
        }
    }
    return false;
}

const std::function<void()> &actionFor(const RuntimeTeardownActions &actions,
                                      RuntimeTeardownStep step) {
    switch (step) {
    case RuntimeTeardownStep::wait_idle: return actions.wait_idle;
    case RuntimeTeardownStep::owner_callbacks: return actions.owner_callbacks;
    case RuntimeTeardownStep::physics: return actions.physics;
    case RuntimeTeardownStep::ecs: return actions.ecs;
    case RuntimeTeardownStep::model_instances: return actions.model_instances;
    case RuntimeTeardownStep::deferred_mutations: return actions.deferred_mutations;
    case RuntimeTeardownStep::pending_events: return actions.pending_events;
    case RuntimeTeardownStep::deletion_queue: return actions.deletion_queue;
    }
    std::terminate();
}

void runProductionStep(RuntimeTeardownStep step) {
    switch (step) {
    case RuntimeTeardownStep::wait_idle:
        if (auto *vulkan = FastModuleContainer::tryGet<VulkanManageCore>())
            vulkan->waitIdle();
        break;
    case RuntimeTeardownStep::owner_callbacks:
        internal::preDestroyAllBehaviorObjectsForTeardown();
        break;
    case RuntimeTeardownStep::physics:
#if PELICAN_WITH_PHYSICS
        if (auto *physics = FastModuleContainer::tryGet<PhysWorld>())
            physics->clear();
#endif
        break;
    case RuntimeTeardownStep::ecs:
        if (auto *ecs = FastModuleContainer::tryGet<ECSCore>())
            ecs->clearEntities();
        break;
    case RuntimeTeardownStep::model_instances:
        if (auto *instances = FastModuleContainer::tryGet<PolygonInstanceContainer>())
            instances->clear();
        break;
    case RuntimeTeardownStep::deferred_mutations:
        (void)internal::drainDeferredBehaviorMutationsForTeardown();
        break;
    case RuntimeTeardownStep::pending_events:
        (void)internal::drainPendingEventsForTeardown();
        break;
    case RuntimeTeardownStep::deletion_queue:
        if (auto *queue = FastModuleContainer::tryGet<DeletionQueue>()) {
            if (FastModuleContainer::phase() ==
                ModuleRuntimePhase::shutting_down) {
                queue->drainForTeardown();
            } else {
                queue->flushAll();
            }
        }
        break;
    }
}

} // namespace

RuntimeTeardownResult
teardownRuntimeNoThrow(const RuntimeTeardownActions &actions) noexcept {
    RuntimeTeardownResult result;
    for (const auto step : runtime_teardown_order) {
        const auto &action = actionFor(actions, step);
        if (action &&
            !cleanupStep(runtimeTeardownStepName(step).data(), action)) {
            result.failed_steps |= runtimeTeardownStepBit(step);
        }
    }
    return result;
}

RuntimeTeardownResult teardownRuntimeNoThrow() noexcept {
    RuntimeTeardownResult result;
    for (const auto step : runtime_teardown_order) {
        if (!cleanupStep(runtimeTeardownStepName(step).data(),
                         [step] { runProductionStep(step); })) {
            result.failed_steps |= runtimeTeardownStepBit(step);
        }
    }
    return result;
}

void requireRuntimeTeardownSuccess(const RuntimeTeardownResult &result) {
    if (result.succeeded()) {
        return;
    }

    std::string message = "runtime teardown failed at";
    for (const auto step : runtime_teardown_order) {
        if (result.failed(step)) {
            message += " ";
            message += runtimeTeardownStepName(step);
        }
    }
    throw std::runtime_error(std::move(message));
}

RuntimeTeardownGuard::~RuntimeTeardownGuard() {
    run();
}

RuntimeTeardownResult RuntimeTeardownGuard::run() noexcept {
    if (completed) {
        return result;
    }
    completed = true;
    if (mode == RuntimeTeardownMode::terminal_shutdown)
        FastModuleContainer::beginShutdown();
    if (actions) {
        result = teardownRuntimeNoThrow(*actions);
    } else {
        result = teardownRuntimeNoThrow();
    }
    return result;
}

} // namespace Pelican
