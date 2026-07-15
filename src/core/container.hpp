#pragma once

#include "./log.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

#define DECLARE_MODULE(name) class name : public ModuleBase<name>
#define GET_MODULE(name) FastModuleContainer::get<name>()

namespace Pelican {

enum class ModuleRuntimePhase {
    booting,
    running,
    shutting_down,
};

struct ModuleDependencyEdge {
    std::string from;
    std::string to;

    bool operator==(const ModuleDependencyEdge &) const = default;
};

struct ModuleGraphSnapshot {
    ModuleRuntimePhase phase = ModuleRuntimePhase::booting;
    bool creation_frozen = false;
    std::vector<std::string> initialized_modules;
    std::vector<std::string> initialized_after_runtime_start;
    std::vector<ModuleDependencyEdge> dependencies;
};

template <class T> struct ModuleBase {
    static std::optional<T> &__get() {
        static std::optional<T> obj;
        return obj;
    }

    static std::atomic_bool &__ready() {
        static std::atomic_bool ready{false};
        return ready;
    }
};

class FastModuleContainer {
    using CleanerFunctionType = void();

    struct CleanerEntry {
        std::type_index id;
        CleanerFunctionType *cleaner;
    };

    static inline std::vector<CleanerEntry> cleaners;
    static inline std::recursive_mutex state_mutex;
    static inline std::optional<std::thread::id> owner_thread;
    static inline bool lifetime_scope_active = false;
    static inline ModuleRuntimePhase runtime_phase = ModuleRuntimePhase::booting;
    static inline bool creation_frozen = false;
    static inline std::vector<std::pair<std::type_index, std::type_index>> dependency_edges;
    static inline std::vector<std::type_index> runtime_initializations;
    static inline thread_local std::vector<std::type_index> construction_stack;

    static std::string constructionCycleMessage(std::type_index repeated) {
        std::string message = "Module construction cycle: ";
        const auto first = std::find(construction_stack.begin(), construction_stack.end(), repeated);
        for (auto current = first; current != construction_stack.end(); ++current) {
            if (current != first) message += " -> ";
            message += current->name();
        }
        if (first != construction_stack.end()) message += " -> ";
        message += repeated.name();
        return message;
    }

    static void recordDependencyLocked(std::type_index dependency) {
        if (construction_stack.empty()) return;
        const auto edge = std::pair{construction_stack.back(), dependency};
        if (std::find(dependency_edges.begin(), dependency_edges.end(), edge) ==
            dependency_edges.end()) {
            dependency_edges.push_back(edge);
        }
    }

    static void recordInitializedDependency(std::type_index dependency) {
        if (construction_stack.empty()) return;
        std::scoped_lock lock{state_mutex};
        recordDependencyLocked(dependency);
    }

    static void requireCreationAllowedLocked(std::type_index id) {
        if (runtime_phase == ModuleRuntimePhase::shutting_down) {
            throw std::logic_error("Module '" + std::string{id.name()} +
                                   "' cannot be initialized during shutdown");
        }
        if (creation_frozen) {
            throw std::logic_error("Module '" + std::string{id.name()} +
                                   "' was first requested after the runtime module graph was frozen");
        }
        const auto current_thread = std::this_thread::get_id();
        if (!owner_thread) {
            owner_thread = current_thread;
        } else if (*owner_thread != current_thread) {
            throw std::logic_error("Module '" + std::string{id.name()} +
                                   "' must be initialized on the module owner thread");
        }
    }

    template <class T> static void destroyModule() noexcept {
        T::__ready().store(false, std::memory_order_release);
        T::__get().reset();
    }

  public:
    FastModuleContainer() {
        std::scoped_lock lock{state_mutex};
        if (lifetime_scope_active) {
            throw std::logic_error("FastModuleContainer lifetime scopes cannot overlap");
        }
        const auto current_thread = std::this_thread::get_id();
        if (owner_thread && *owner_thread != current_thread) {
            throw std::logic_error("FastModuleContainer must be owned by the module initialization thread");
        }
        owner_thread = current_thread;
        lifetime_scope_active = true;
    }

    FastModuleContainer(const FastModuleContainer &) = delete;
    FastModuleContainer &operator=(const FastModuleContainer &) = delete;
    FastModuleContainer(FastModuleContainer &&) = delete;
    FastModuleContainer &operator=(FastModuleContainer &&) = delete;

    template <class T> static bool isInitialized() noexcept {
        return T::__ready().load(std::memory_order_acquire);
    }

    template <class T> static T *tryGet() noexcept {
        if (!T::__ready().load(std::memory_order_acquire)) return nullptr;
        return &T::__get().value();
    }

    template <class T> static T &get() {
        const auto id = std::type_index{typeid(T)};
        if (T::__ready().load(std::memory_order_acquire)) {
            recordInitializedDependency(id);
            return T::__get().value();
        }

        std::scoped_lock lock{state_mutex};
        std::optional<T> &obj_ref = T::__get();
        if (T::__ready().load(std::memory_order_acquire)) {
            recordDependencyLocked(id);
            return obj_ref.value();
        }

        recordDependencyLocked(id);
        if (std::find(construction_stack.begin(), construction_stack.end(), id) !=
            construction_stack.end()) {
            throw std::logic_error(constructionCycleMessage(id));
        }
        requireCreationAllowedLocked(id);

        if (logger != nullptr) {
            LOG_INFO(logger, "Module [{}] initializing...", id.name());
        }
        construction_stack.push_back(id);
        struct ConstructionScope {
            ~ConstructionScope() { FastModuleContainer::construction_stack.pop_back(); }
        } construction_scope;

        obj_ref.emplace();
        const auto initialized_during_runtime = runtime_phase == ModuleRuntimePhase::running;
        try {
            cleaners.push_back({id, &destroyModule<T>});
            if (initialized_during_runtime &&
                std::find(runtime_initializations.begin(), runtime_initializations.end(), id) ==
                    runtime_initializations.end()) {
                runtime_initializations.push_back(id);
            }
        } catch (...) {
            if (!cleaners.empty() && cleaners.back().id == id) cleaners.pop_back();
            obj_ref.reset();
            throw;
        }
        T::__ready().store(true, std::memory_order_release);
        if (logger != nullptr) {
            LOG_INFO(logger, "Module [{}] initialized", id.name());
        }
        return obj_ref.value();
    }

    static void freezeCreation() {
        std::scoped_lock lock{state_mutex};
        runtime_phase = ModuleRuntimePhase::running;
        creation_frozen = true;
    }

    static void enterRunningPhase() {
        std::scoped_lock lock{state_mutex};
        if (runtime_phase == ModuleRuntimePhase::shutting_down) {
            throw std::logic_error("module runtime cannot re-enter the running phase during shutdown");
        }
        runtime_phase = ModuleRuntimePhase::running;
    }

    static void beginShutdown() noexcept {
        std::scoped_lock lock{state_mutex};
        runtime_phase = ModuleRuntimePhase::shutting_down;
        creation_frozen = true;
    }

    static ModuleRuntimePhase phase() noexcept {
        std::scoped_lock lock{state_mutex};
        return runtime_phase;
    }

    static bool isCreationFrozen() noexcept {
        std::scoped_lock lock{state_mutex};
        return creation_frozen;
    }

    static ModuleGraphSnapshot graphSnapshot() {
        std::scoped_lock lock{state_mutex};
        ModuleGraphSnapshot result;
        result.phase = runtime_phase;
        result.creation_frozen = creation_frozen;
        result.initialized_modules.reserve(cleaners.size());
        for (const auto &entry : cleaners) result.initialized_modules.emplace_back(entry.id.name());
        std::sort(result.initialized_modules.begin(), result.initialized_modules.end());
        result.initialized_modules.erase(
            std::unique(result.initialized_modules.begin(), result.initialized_modules.end()),
            result.initialized_modules.end());

        result.initialized_after_runtime_start.reserve(runtime_initializations.size());
        for (const auto id : runtime_initializations)
            result.initialized_after_runtime_start.emplace_back(id.name());
        std::sort(result.initialized_after_runtime_start.begin(),
                  result.initialized_after_runtime_start.end());

        result.dependencies.reserve(dependency_edges.size());
        for (const auto &[from, to] : dependency_edges) {
            result.dependencies.push_back({from.name(), to.name()});
        }
        std::sort(result.dependencies.begin(), result.dependencies.end(),
                  [](const auto &left, const auto &right) {
                      return std::pair{left.from, left.to} < std::pair{right.from, right.to};
                  });
        return result;
    }

    ~FastModuleContainer() noexcept {
        std::scoped_lock lock{state_mutex};
        runtime_phase = ModuleRuntimePhase::shutting_down;
        creation_frozen = true;
        while (!cleaners.empty()) {
            const auto [id, cleaner] = cleaners.back();
            if (logger != nullptr) {
                LOG_INFO(logger, "Module [{}] destroying...", id.name());
            }
            cleaner();
            cleaners.pop_back();
            if (logger != nullptr) {
                LOG_INFO(logger, "Module [{}] destroyed", id.name());
            }
        }
        dependency_edges.clear();
        runtime_initializations.clear();
        construction_stack.clear();
        owner_thread.reset();
        lifetime_scope_active = false;
        runtime_phase = ModuleRuntimePhase::booting;
        creation_frozen = false;
    }
};

} // namespace Pelican
