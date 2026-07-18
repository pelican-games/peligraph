#include "gamelogicreload.hpp"
#include "behaviorarena.hpp"

#include "../animation/animationservice.hpp"
#if PELICAN_WITH_PHYSICS
#include "../phys/physicsruntime.hpp"
#endif

#include "../appflow/teardown.hpp"
#include "../launchconfig.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/scene.hpp"
#include "../log.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../userpublic/details/behavior/registerer.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../userpublic/gamelogic.hpp"

#include <chrono>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace Pelican {

namespace internal {

void releaseGameLogicRegistrations(RegistrationOwner owner) noexcept {
    // Live behavior instances and their callbacks must be gone while the owner
    // DLL is still loaded. Registry entries are erased only afterwards.
    releaseBehaviorOwner(owner);
    Animation::releaseAnimationOwner(owner);
#if PELICAN_WITH_PHYSICS
    physics_internal::releaseProviderOwner(owner);
#endif
    unregisterGameSystems(owner);
    unregisterEvents(owner);
    unregisterBehaviors(owner);
}

} // namespace internal

namespace {

std::atomic_bool reload_in_progress{false};

class ReloadStateGuard {
  public:
    ReloadStateGuard() { reload_in_progress.store(true, std::memory_order_release); }
    ~ReloadStateGuard() { reload_in_progress.store(false, std::memory_order_release); }
};

std::string platformLoadError() {
#ifdef _WIN32
    const auto code = GetLastError();
    // System messages follow the machine locale, while JSON-RPC is UTF-8.
    return "Windows loader error code " + std::to_string(code);
#else
    const char *message = dlerror();
    return message != nullptr ? message : "unknown dynamic-loader error";
#endif
}

void removeFileNoThrow(const std::filesystem::path &path) noexcept {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto pdb = path;
    pdb.replace_extension(".pdb");
    std::filesystem::remove(pdb, ec);
}

} // namespace

GameLogicReloader::~GameLogicReloader() {
    shutdown();
}

std::filesystem::path GameLogicReloader::makeShadowCopy(const char *purpose) {
    std::error_code ec;
    std::filesystem::create_directories(shadow_directory, ec);
    if (ec) {
        throw std::runtime_error("game logic DLL '" + source_path.string() +
                                 "': cannot create shadow directory: " + ec.message());
    }
    const auto name = source_path.stem().string() + "_" + purpose + "_" +
                      std::to_string(++copy_sequence) + source_path.extension().string();
    const auto copy = shadow_directory / name;
    std::filesystem::copy_file(source_path, copy, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw std::runtime_error("game logic DLL '" + source_path.string() +
                                 "': shadow copy failed: " + ec.message());
    }

    auto source_pdb = source_path;
    source_pdb.replace_extension(".pdb");
    if (std::filesystem::is_regular_file(source_pdb, ec)) {
        auto copy_pdb = copy;
        copy_pdb.replace_extension(".pdb");
        ec.clear();
        std::filesystem::copy_file(source_pdb, copy_pdb,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec && logger != nullptr) {
            LOG_WARNING(logger, "game logic PDB '{}' shadow copy failed: {}", source_pdb.string(), ec.message());
        }
    }
    return copy;
}

std::optional<GameLogicReloader::LoadedLibrary>
GameLogicReloader::loadCopy(const std::filesystem::path &path, internal::RegistrationOwner owner,
                           std::string &error) {
    internal::ScopedRegistrationOwner registration_scope{owner};
#ifdef _WIN32
    const auto handle = LoadLibraryW(path.c_str());
#else
    const auto handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (handle == nullptr) {
        internal::releaseGameLogicRegistrations(owner);
        error = "game logic DLL '" + source_path.string() + "' load failed: " + platformLoadError();
        return std::nullopt;
    }

#ifdef _WIN32
    const auto abi = reinterpret_cast<GameLogicAbiVersionFn>(GetProcAddress(handle, gameLogicAbiSymbol));
#else
    const auto abi = reinterpret_cast<GameLogicAbiVersionFn>(dlsym(handle, gameLogicAbiSymbol));
#endif
    if (abi == nullptr) {
        error = "game logic DLL '" + source_path.string() + "' is missing ABI symbol '" +
                gameLogicAbiSymbol + "'";
    } else if (const auto candidate_abi = abi(); candidate_abi != gameLogicAbiVersion) {
        error = "game logic DLL '" + source_path.string() + "' ABI version " +
                std::to_string(candidate_abi) + " does not match engine ABI version " +
                std::to_string(gameLogicAbiVersion);
    }
    if (!error.empty()) {
        internal::releaseGameLogicRegistrations(owner);
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        return std::nullopt;
    }
    return LoadedLibrary{.handle = handle, .path = path, .owner = owner};
}

void GameLogicReloader::unload(LoadedLibrary &library) noexcept {
    internal::releaseGameLogicRegistrations(library.owner);
    if (library.handle != nullptr) {
#ifdef _WIN32
        FreeLibrary(static_cast<HMODULE>(library.handle));
#else
        dlclose(library.handle);
#endif
        library.handle = nullptr;
    }
}

bool GameLogicReloader::validateCandidate(const std::filesystem::path &path, std::string &error) {
    const auto owner = internal::allocateRegistrationOwner();
    auto candidate = loadCopy(path, owner, error);
    if (!candidate) return false;
    try {
        if (active) {
            const nlohmann::json *authoring_scenes = nullptr;
            if (const auto *config =
                    FastModuleContainer::tryGet<ProjectBasicConfig>()) {
                authoring_scenes = &config->sceneDocument().scenesJson();
            }
            internal::validateBehaviorReload(active->owner, candidate->owner,
                                             authoring_scenes);
        }
    } catch (const std::exception &validation_error) {
        error = validation_error.what();
        unload(*candidate);
        return false;
    }
    unload(*candidate);
    return true;
}

bool GameLogicReloader::initialize(const std::filesystem::path &source) {
    shutdown();
    source_path = std::filesystem::absolute(source).lexically_normal();
    shadow_directory = source_path.parent_path() / ".pelican-hot-reload";
    std::error_code ec;
    observed_write_time = std::filesystem::last_write_time(source_path, ec);
    if (ec) {
        last_error = "game logic DLL '" + source_path.string() + "' is unavailable: " + ec.message();
        LOG_ERROR(logger, "{}", last_error);
        return false;
    }

    try {
        const auto copy = makeShadowCopy("initial");
        std::string error;
        const auto owner = internal::allocateRegistrationOwner();
        active = loadCopy(copy, owner, error);
        if (!active) {
            removeFileNoThrow(copy);
            last_error = std::move(error);
            LOG_ERROR(logger, "{}", last_error);
            return false;
        }
#if PELICAN_WITH_PHYSICS
        physics_internal::activateProviderOwner(owner);
#endif
        generation = 1;
        last_error.clear();
        LOG_INFO(logger, "game logic DLL loaded: source='{}' copy='{}' ABI={} systems={}",
                 source_path.string(), copy.string(), gameLogicAbiVersion,
                 internal::gameSystemRegistrationCount(owner));
        return true;
    } catch (const std::exception &error) {
        last_error = error.what();
        LOG_ERROR(logger, "{}", last_error);
        return false;
    }
}

bool GameLogicReloader::reloadTransaction(const ResetFn &teardown, const ResetFn &rebuild) {
    std::filesystem::path candidate_path;
    try {
        candidate_path = makeShadowCopy("candidate");
    } catch (const std::exception &error) {
        last_error = error.what();
        LOG_ERROR(logger, "{}; keeping previous game logic DLL", last_error);
        return false;
    }

    std::string validation_error;
    if (!validateCandidate(candidate_path, validation_error)) {
        removeFileNoThrow(candidate_path);
        last_error = std::move(validation_error);
        LOG_ERROR(logger, "{}; keeping previous game logic DLL", last_error);
        return false;
    }

    ReloadStateGuard reload_state;
    auto previous = std::move(active);
    bool previous_unloaded = false;
    active.reset();
    std::string reload_error;
    try {
        teardown();
        if (previous) {
            unload(*previous);
            previous_unloaded = true;
        }

        std::string load_error;
        const auto owner = internal::allocateRegistrationOwner();
        active = loadCopy(candidate_path, owner, load_error);
        if (!active) throw std::runtime_error(load_error);
#if PELICAN_WITH_PHYSICS
        physics_internal::activateProviderOwner(owner);
#endif
        rebuild();

        ++generation;
        last_error.clear();
        if (previous) removeFileNoThrow(previous->path);
        LOG_INFO(logger, "game logic DLL reloaded: source='{}' generation={} systems={}",
                 source_path.string(), generation, internal::gameSystemRegistrationCount(owner));
        return true;
    } catch (const std::exception &error) {
        // Copy the message and leave the handler before unloading a DLL that
        // may own the active exception's destructor/unwind metadata.
        reload_error = error.what();
    } catch (...) {
        reload_error = "non-standard exception";
    }

    if (active) {
        try { teardown(); } catch (...) {}
        unload(*active);
        active.reset();
    }
    removeFileNoThrow(candidate_path);

    std::string rollback_error;
    if (previous) {
        bool activate_rollback_owner = false;
        if (previous_unloaded) {
            const auto rollback_owner = internal::allocateRegistrationOwner();
            active = loadCopy(previous->path, rollback_owner, rollback_error);
            activate_rollback_owner = active.has_value();
        } else {
            // Teardown failed before the old DLL was unloaded. Reuse that
            // exact handle/owner instead of loading a second copy whose
            // static registrations would not have a well-defined owner.
            active = std::move(previous);
        }
        if (active) {
#if PELICAN_WITH_PHYSICS
            if (activate_rollback_owner) {
                physics_internal::activateProviderOwner(active->owner);
            }
#endif
            bool rollback_rebuild_failed = false;
            try {
                rebuild();
            } catch (const std::exception &error) {
                rollback_error = error.what();
                rollback_rebuild_failed = true;
            } catch (...) {
                rollback_error = "non-standard exception";
                rollback_rebuild_failed = true;
            }
            if (rollback_rebuild_failed) {
                try { teardown(); } catch (...) {}
                unload(*active);
                active.reset();
            }
        }
    }
    last_error = "game logic DLL '" + source_path.string() + "' reload failed: " + reload_error;
    if (!rollback_error.empty()) last_error += "; rollback failed: " + rollback_error;
    LOG_ERROR(logger, "{}", last_error);
    return false;
}

GameLogicReloadAttempt GameLogicReloader::reloadNowAttempt(const ResetFn &teardown,
                                                           const ResetFn &rebuild) {
    if (source_path.empty()) {
        last_error = "game logic DLL reload requested, but no DLL is configured";
        return {.attempted = true, .error = last_error};
    }
    // A forced/manual reload acknowledges the current source version too. This
    // prevents the next automatic poll from applying the same DLL a second time.
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(source_path, ec);
    if (!ec) observed_write_time = write_time;

    const bool committed = reloadTransaction(teardown, rebuild);
    return {
        .attempted = true,
        .committed = committed,
        .error = committed ? std::string{} : last_error,
    };
}

GameLogicReloadAttempt GameLogicReloader::pollAttempt(const ResetFn &teardown,
                                                      const ResetFn &rebuild, bool force) {
    if (source_path.empty()) return {};
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(source_path, ec);
    if (ec) {
        last_error = "game logic DLL '" + source_path.string() + "' stat failed: " + ec.message();
        return {.attempted = true, .error = last_error};
    }
    if (!force && write_time == observed_write_time) return {};
    observed_write_time = write_time;
    const bool committed = reloadTransaction(teardown, rebuild);
    return {
        .attempted = true,
        .committed = committed,
        .error = committed ? std::string{} : last_error,
    };
}

bool GameLogicReloader::reloadNow(const ResetFn &teardown, const ResetFn &rebuild) {
    return reloadNowAttempt(teardown, rebuild).committed;
}

bool GameLogicReloader::poll(const ResetFn &teardown, const ResetFn &rebuild, bool force) {
    return pollAttempt(teardown, rebuild, force).committed;
}

void GameLogicReloader::shutdown() noexcept {
    if (active) {
        const auto path = active->path;
        unload(*active);
        active.reset();
        removeFileNoThrow(path);
    }
    source_path.clear();
    last_error.clear();
    generation = 0;
}

GameLogicReloadStatus GameLogicReloader::status() const {
    const auto owner = active ? active->owner : internal::engineRegistrationOwner;
    return GameLogicReloadStatus{
        .configured = !source_path.empty(),
        .loaded = active.has_value(),
        .reloading = isGameLogicReloadInProgress(),
        .generation = generation,
        .system_count = active ? internal::gameSystemRegistrationCount(owner) : 0,
        .source = source_path,
        .loaded_copy = active ? active->path : std::filesystem::path{},
        .last_error = last_error,
    };
}

bool isGameLogicReloadInProgress() noexcept {
    return reload_in_progress.load(std::memory_order_acquire);
}

namespace {
void runtimeTeardown() {
    RuntimeTeardownGuard guard;
    guard.run();
}

void rebuildCurrentScene() {
    auto &scene_loader = GET_MODULE(SceneLoader);
    auto scene = scene_loader.currentScene();
    if (!scene.empty()) scene_loader.load(std::move(scene));
}

bool deterministicDriverActive() {
    const auto *config = FastModuleContainer::tryGet<EngineLaunchConfig>();
    return config != nullptr && (config->input_replay || config->golden_mode);
}
} // namespace

bool initializeConfiguredGameLogic() {
    const auto &config = GET_MODULE(EngineLaunchConfig);
    if (!config.game_logic_dll) return true;
    return GET_MODULE(GameLogicReloader).initialize(*config.game_logic_dll);
}

GameLogicReloadAttempt reloadConfiguredGameLogicAttempt(bool force) {
    if (deterministicDriverActive()) {
        throw std::runtime_error("game logic DLL reload rejected while replay/golden driver is active");
    }
    auto &reloader = GET_MODULE(GameLogicReloader);
    if (force) return reloader.reloadNowAttempt(runtimeTeardown, rebuildCurrentScene);
    return reloader.pollAttempt(runtimeTeardown, rebuildCurrentScene, false);
}

GameLogicReloadAttempt pollConfiguredGameLogicAttempt(bool force) {
    if (deterministicDriverActive()) return {};
    auto *reloader = FastModuleContainer::tryGet<GameLogicReloader>();
    if (reloader == nullptr) return {};
    return reloader->pollAttempt(runtimeTeardown, rebuildCurrentScene, force);
}

bool reloadConfiguredGameLogic(bool force) {
    return reloadConfiguredGameLogicAttempt(force).committed;
}

void pollConfiguredGameLogic(bool force) {
    (void)pollConfiguredGameLogicAttempt(force);
}

void shutdownConfiguredGameLogic() noexcept {
    if (auto *reloader = FastModuleContainer::tryGet<GameLogicReloader>()) reloader->shutdown();
}

GameLogicReloadStatus configuredGameLogicStatus() {
    const auto *reloader = FastModuleContainer::tryGet<GameLogicReloader>();
    return reloader == nullptr ? GameLogicReloadStatus{} : reloader->status();
}

} // namespace Pelican
