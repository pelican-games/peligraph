#include "../src/core/appflow/teardown.hpp"
#include "../src/core/container.hpp"
#include "../src/core/gamelogic/behaviorarena.hpp"
#include "../src/core/userpublic/details/event/registerer.hpp"
#include "../src/core/vkcore/deferredcallback.hpp"
#include "../src/core/vkcore/deletionqueue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

struct LifetimeTeardownEvent {
    int value = 0;
};

std::vector<std::string> module_destruction_trace;

DECLARE_MODULE(LifetimePermutationLeaf) {
  public:
    ~LifetimePermutationLeaf() { module_destruction_trace.emplace_back("leaf"); }
};

DECLARE_MODULE(LifetimePermutationOwner) {
    struct DeferredProbe {
        LifetimePermutationOwner *owner = nullptr;
        DeferredCallbackLifetime::Callback callback;
        int *callback_count = nullptr;
        int *invalid_owner_count = nullptr;

        DeferredProbe(LifetimePermutationOwner &value,
                      DeferredCallbackLifetime::Callback callback_handle,
                      int &callbacks, int &invalid_owners)
            : owner{&value}, callback{std::move(callback_handle)},
              callback_count{&callbacks}, invalid_owner_count{&invalid_owners} {}
        DeferredProbe(const DeferredProbe &) = delete;
        DeferredProbe &operator=(const DeferredProbe &) = delete;
        DeferredProbe(DeferredProbe &&other) noexcept
            : owner{std::exchange(other.owner, nullptr)},
              callback{std::move(other.callback)},
              callback_count{other.callback_count},
              invalid_owner_count{other.invalid_owner_count} {}
        DeferredProbe &operator=(DeferredProbe &&) = delete;
        ~DeferredProbe() {
            if (owner == nullptr) return;
            const auto lease = callback.acquire();
            if (!lease) return;
            if (!owner->alive) ++*invalid_owner_count;
            ++*callback_count;
        }
    };

    DeferredCallbackLifetime callback_lifetime;
    bool alive = true;

  public:
    LifetimePermutationOwner() { (void)GET_MODULE(LifetimePermutationLeaf); }
    ~LifetimePermutationOwner() {
        callback_lifetime.closeAndWait();
        alive = false;
        module_destruction_trace.emplace_back("owner");
    }

    DeferredProbe deferredProbe(int &callbacks, int &invalid_owners) {
        return DeferredProbe{*this, callback_lifetime.callback(), callbacks,
                             invalid_owners};
    }
};

DECLARE_MODULE(LifetimePermutationQueue) {
  public:
    DeletionQueueCore core{2, [] {}};
    ~LifetimePermutationQueue() {
        module_destruction_trace.emplace_back("queue");
    }
};

enum class RequestedModule : unsigned char {
    leaf,
    owner,
    queue,
    deferred_mutations,
};

void requestModule(RequestedModule module) {
    switch (module) {
    case RequestedModule::leaf:
        (void)GET_MODULE(LifetimePermutationLeaf);
        break;
    case RequestedModule::owner:
        (void)GET_MODULE(LifetimePermutationOwner);
        break;
    case RequestedModule::queue:
        (void)GET_MODULE(LifetimePermutationQueue);
        break;
    case RequestedModule::deferred_mutations:
        (void)GET_MODULE(BehaviorAttachmentArena);
        break;
    }
}

struct TeardownObservation {
    std::vector<std::string> steps;
    internal::EventQueueDrainResult events;
    std::size_t deferred_mutations = 0;
};

RuntimeTeardownActions observedActions(TeardownObservation &observation,
                                       std::string fault_step = {},
                                       bool close_deletion_queue = true) {
    const auto observe = [&observation, fault_step = std::move(fault_step)](
                             std::string name, std::function<void()> operation) {
        return [&observation, fault_step, name = std::move(name),
                operation = std::move(operation)] {
            observation.steps.push_back(name);
            operation();
            if (fault_step == name) {
                throw std::runtime_error("injected teardown fault at " + name);
            }
        };
    };

    return RuntimeTeardownActions{
        .wait_idle = observe("wait-idle", [] {}),
        .owner_callbacks = observe("owner-callbacks", [] {
            internal::preDestroyAllBehaviorObjectsForTeardown();
        }),
        .physics = observe("physics", [] {}),
        .ecs = observe("ecs", [] {}),
        .model_instances = observe("model-instances", [] {}),
        .deferred_mutations = observe("deferred-mutations", [&observation] {
            observation.deferred_mutations =
                internal::drainDeferredBehaviorMutationsForTeardown();
        }),
        .pending_events = observe("pending-events", [&observation] {
            observation.events = internal::drainPendingEventsForTeardown();
        }),
        .deletion_queue = observe("deletion-queue", [close_deletion_queue] {
            if (auto *queue =
                    FastModuleContainer::tryGet<LifetimePermutationQueue>()) {
                if (close_deletion_queue) {
                    queue->core.drainForTeardown();
                } else {
                    queue->core.flushAll();
                }
            }
        }),
    };
}

void seedExplicitQueues(int &callback_count, int &invalid_owner_count) {
    auto &events = internal::getEventRegisterer();
    events.emit(LifetimeTeardownEvent{1});
    events.freezePendingEventsForFrame();
    events.emit(LifetimeTeardownEvent{2});

    GET_MODULE(BehaviorAttachmentArena).deferCreateObject({});
    auto &owner = GET_MODULE(LifetimePermutationOwner);
    GET_MODULE(LifetimePermutationQueue)
        .core.defer(owner.deferredProbe(callback_count, invalid_owner_count));
}

const std::vector<std::string> normative_teardown_steps = [] {
    std::vector<std::string> result;
    result.reserve(runtime_teardown_order.size());
    for (const auto step : runtime_teardown_order)
        result.emplace_back(runtimeTeardownStepName(step));
    return result;
}();

void requireOwnerBeforeLeaf() {
    const auto owner = std::find(module_destruction_trace.begin(),
                                 module_destruction_trace.end(), "owner");
    const auto leaf = std::find(module_destruction_trace.begin(),
                                module_destruction_trace.end(), "leaf");
    REQUIRE(owner != module_destruction_trace.end());
    REQUIRE(leaf != module_destruction_trace.end());
    REQUIRE(owner < leaf);
}

} // namespace

PELICAN_REGISTER_EVENT(LifetimeTeardownEvent);

TEST_CASE("Exceptional teardown drains every explicit queue in normative order",
          "[wp171][teardown][fault-injection]") {
    for (const auto &fault_step : normative_teardown_steps) {
        CAPTURE(fault_step);
        internal::clearPendingEvents();
        module_destruction_trace.clear();
        int callback_count = 0;
        int invalid_owner_count = 0;
        TeardownObservation observation;
        {
            FastModuleContainer modules;
            RuntimeTeardownGuard teardown{
                observedActions(observation, fault_step)};
            (void)GET_MODULE(LifetimePermutationOwner);
            (void)GET_MODULE(LifetimePermutationQueue);
            (void)GET_MODULE(BehaviorAttachmentArena);
            seedExplicitQueues(callback_count, invalid_owner_count);

            teardown.run();

            REQUIRE(FastModuleContainer::phase() ==
                    ModuleRuntimePhase::shutting_down);
            REQUIRE(GET_MODULE(LifetimePermutationQueue).core.pendingCount() == 0);
            REQUIRE_FALSE(GET_MODULE(LifetimePermutationQueue)
                              .core.acceptingResources());
            REQUIRE(GET_MODULE(BehaviorAttachmentArena)
                        .deferredMutationCountForTesting() == 0);
        }

        REQUIRE(observation.steps == normative_teardown_steps);
        REQUIRE(observation.events.pending == 1);
        REQUIRE(observation.events.frozen == 1);
        REQUIRE(observation.deferred_mutations == 1);
        REQUIRE(internal::getEventRegisterer().pendingEventCount() == 0);
        REQUIRE(callback_count == 1);
        REQUIRE(invalid_owner_count == 0);
        requireOwnerBeforeLeaf();
    }
}

TEST_CASE("Runtime reset drains work without entering terminal shutdown",
          "[wp171][teardown][runtime-reset]") {
    internal::clearPendingEvents();
    module_destruction_trace.clear();
    int callback_count = 0;
    int invalid_owner_count = 0;
    TeardownObservation observation;

    {
        FastModuleContainer modules;
        RuntimeTeardownGuard teardown{
            observedActions(observation, {}, false),
            RuntimeTeardownMode::runtime_reset};
        seedExplicitQueues(callback_count, invalid_owner_count);
        FastModuleContainer::enterRunningPhase();

        teardown.run();

        REQUIRE(observation.steps == normative_teardown_steps);
        REQUIRE(FastModuleContainer::phase() == ModuleRuntimePhase::running);
        auto &queue = GET_MODULE(LifetimePermutationQueue).core;
        REQUIRE(queue.acceptingResources());
        queue.defer([] {});
        REQUIRE(queue.pendingCount() == 1);
        queue.flushAll();
        REQUIRE(queue.pendingCount() == 0);
    }

    REQUIRE(callback_count == 1);
    REQUIRE(invalid_owner_count == 0);
    requireOwnerBeforeLeaf();
}

TEST_CASE("Allowed module creation permutations finish on normal and exceptional exits",
          "[wp171][teardown][module-permutation]") {
    std::array order{
        RequestedModule::leaf,
        RequestedModule::owner,
        RequestedModule::queue,
        RequestedModule::deferred_mutations,
    };

    do {
        for (const bool inject_exception : {false, true}) {
            CAPTURE(order, inject_exception);
            internal::clearPendingEvents();
            module_destruction_trace.clear();
            int callback_count = 0;
            int invalid_owner_count = 0;
            TeardownObservation observation;
            bool caught = false;
            try {
                FastModuleContainer modules;
                RuntimeTeardownGuard teardown{observedActions(observation)};
                for (const auto module : order) requestModule(module);
                seedExplicitQueues(callback_count, invalid_owner_count);
                if (inject_exception) {
                    throw std::runtime_error("injected runtime fatal");
                }
                teardown.run();
            } catch (const std::runtime_error &) {
                caught = true;
            }

            REQUIRE(caught == inject_exception);
            REQUIRE(observation.steps == normative_teardown_steps);
            REQUIRE(observation.events.pending == 1);
            REQUIRE(observation.events.frozen == 1);
            REQUIRE(observation.deferred_mutations == 1);
            REQUIRE(internal::getEventRegisterer().pendingEventCount() == 0);
            REQUIRE(callback_count == 1);
            REQUIRE(invalid_owner_count == 0);
            requireOwnerBeforeLeaf();
        }
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("Startup exceptions teardown only the modules and queues already published",
          "[wp171][teardown][startup-fault]") {
    const std::array order{
        RequestedModule::owner,
        RequestedModule::queue,
        RequestedModule::deferred_mutations,
        RequestedModule::leaf,
    };

    for (std::size_t initialized = 0; initialized <= order.size(); ++initialized) {
        CAPTURE(initialized);
        internal::clearPendingEvents();
        module_destruction_trace.clear();
        TeardownObservation observation;
        bool caught = false;
        try {
            FastModuleContainer modules;
            RuntimeTeardownGuard teardown{observedActions(observation)};
            internal::getEventRegisterer().emit(LifetimeTeardownEvent{3});
            for (std::size_t index = 0; index < initialized; ++index) {
                requestModule(order[index]);
            }
            throw std::runtime_error("injected startup failure");
        } catch (const std::runtime_error &) {
            caught = true;
        }

        REQUIRE(caught);
        REQUIRE(observation.steps == normative_teardown_steps);
        REQUIRE(observation.events.pending == 1);
        REQUIRE(observation.events.frozen == 0);
        REQUIRE(internal::getEventRegisterer().pendingEventCount() == 0);
        if (initialized != 0) requireOwnerBeforeLeaf();
    }
}

} // namespace Pelican
