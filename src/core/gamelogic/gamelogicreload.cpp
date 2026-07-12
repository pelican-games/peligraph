#include "gamelogicreload.hpp"

#include "../appflow/teardown.hpp"
#include "../launchconfig.hpp"
#include "../loader/scene.hpp"
#include "../log.hpp"
#include "../userpublic/details/event/registerer.hpp"
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
        internal::unregisterGameSystems(owner);
        internal::unregisterEvents(owner);
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
        internal::unregisterGameSystems(owner);
        internal::unregisterEvents(owner);
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
    internal::unregisterGameSystems(library.owner);
    internal::unregisterEvents(library.owner);
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
    active.reset();
    try {
        teardown();
        if (previous) unload(*previous);

        std::string load_error;
        const auto owner = internal::allocateRegistrationOwner();
        active = loadCopy(candidate_path, owner, load_error);
        if (!active) throw std::runtime_error(load_error);
        rebuild();

        ++generation;
        last_error.clear();
        if (previous) removeFileNoThrow(previous->path);
        LOG_INFO(logger, "game logic DLL reloaded: source='{}' generation={} systems={}",
                 source_path.string(), generation, internal::gameSystemRegistrationCount(owner));
        return true;
    } catch (const std::exception &error) {
        const std::string reload_error = error.what();
        if (active) {
            try { teardown(); } catch (...) {}
            unload(*active);
            active.reset();
        }
        removeFileNoThrow(candidate_path);

        std::string rollback_error;
        if (previous) {
            const auto rollback_owner = internal::allocateRegistrationOwner();
            active = loadCopy(previous->path, rollback_owner, rollback_error);
            if (active) {
                try {
                    rebuild();
                } catch (const std::exception &rebuild_error) {
                    rollback_error = rebuild_error.what();
                }
            }
        }
        last_error = "game logic DLL '" + source_path.string() + "' reload failed: " + reload_error;
        if (!rollback_error.empty()) last_error += "; rollback failed: " + rollback_error;
        LOG_ERROR(logger, "{}", last_error);
        return false;
    }
}

bool GameLogicReloader::reloadNow(const ResetFn &teardown, const ResetFn &rebuild) {
    if (source_path.empty()) {
        last_error = "game logic DLL reload requested, but no DLL is configured";
        return false;
    }
    return reloadTransaction(teardown, rebuild);
}

bool GameLogicReloader::poll(const ResetFn &teardown, const ResetFn &rebuild, bool force) {
    if (source_path.empty()) return false;
    std::error_code ec;
    const auto write_time = std::filesystem::last_write_time(source_path, ec);
    if (ec) {
        last_error = "game logic DLL '" + source_path.string() + "' stat failed: " + ec.message();
        return false;
    }
    if (!force && write_time == observed_write_time) return false;
    observed_write_time = write_time;
    return reloadNow(teardown, rebuild);
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
    if (!EngineLaunchConfig::__get().has_value()) return false;
    const auto &config = *EngineLaunchConfig::__get();
    return config.input_replay || config.golden_mode;
}
} // namespace

bool initializeConfiguredGameLogic() {
    const auto &config = GET_MODULE(EngineLaunchConfig);
    if (!config.game_logic_dll) return true;
    return GET_MODULE(GameLogicReloader).initialize(*config.game_logic_dll);
}

bool reloadConfiguredGameLogic(bool force) {
    if (deterministicDriverActive()) {
        throw std::runtime_error("game logic DLL reload rejected while replay/golden driver is active");
    }
    auto &reloader = GET_MODULE(GameLogicReloader);
    if (force) return reloader.reloadNow(runtimeTeardown, rebuildCurrentScene);
    return reloader.poll(runtimeTeardown, rebuildCurrentScene, false);
}

void pollConfiguredGameLogic(bool force) {
    if (deterministicDriverActive()) return;
    auto &reloader = GET_MODULE(GameLogicReloader);
    (void)reloader.poll(runtimeTeardown, rebuildCurrentScene, force);
}

void shutdownConfiguredGameLogic() noexcept {
    if (GameLogicReloader::__get().has_value()) GameLogicReloader::__get()->shutdown();
}

GameLogicReloadStatus configuredGameLogicStatus() {
    if (!GameLogicReloader::__get().has_value()) return {};
    return GameLogicReloader::__get()->status();
}

} // namespace Pelican
