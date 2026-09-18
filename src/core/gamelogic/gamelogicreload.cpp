#include "gamelogicreload.hpp"
#include "behaviorreloadvalidation.hpp"
#include "behaviorarena.hpp"
#include "gamelogicimports.hpp"

#if PELICAN_WITH_ANIMATION
#include "../animation/animationservice.hpp"
#endif
#if PELICAN_WITH_PHYSICS
#include "../phys/physicsruntime.hpp"
#endif

#include "../appflow/teardown.hpp"
#include "../launchconfig.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/scene.hpp"
#include "../log.hpp"
#include "../renderer/renderpolicyregistry.hpp"
#include "../renderingpass/passimplementationregistry.hpp"
#include "../renderingpass/graphtransformregistry.hpp"
#include "../renderingpass/renderstrategyregistry.hpp"
#include "../renderingpass/subgraphreplacementregistry.hpp"
#include "../renderingpass/vulkannativescopeexecutor.hpp"
#include "../userpublic/details/event/registerer.hpp"
#include "../userpublic/details/behavior/registerer.hpp"
#include "../userpublic/details/component/registerer.hpp"
#include "../userpublic/details/system/registerer.hpp"
#include "../userpublic/gamelogic.hpp"

#include <chrono>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

// These values are ABI. They are pinned by
// test/fixtures/userpublic_frozen/{v1,v2}/render. RenderPolicy::Status is
// offset by one from the four provider-ABI enums (buffer_too_small = 1); never
// static_cast or memcpy between them. Change these asserts only together with a
// new ABI version.
static_assert(
    static_cast<std::uint32_t>(Pelican::RenderPolicy::Status::buffer_too_small)
    == 1u);
static_assert(
    static_cast<std::uint32_t>(Pelican::RenderPolicy::Status::invalid_argument)
    == 2u);
static_assert(
    static_cast<std::uint32_t>(Pelican::RenderPolicy::Status::unavailable)
    == 11u);
static_assert(
    static_cast<std::uint32_t>(Pelican::RenderPass::Status::invalid_argument)
    == 1u);
static_assert(
    static_cast<std::uint32_t>(Pelican::RenderPass::Status::unavailable)
    == 10u);
static_assert(
    static_cast<std::uint32_t>(
        Pelican::RenderGraphTransform::Status::invalid_argument) == 1u);
static_assert(
    static_cast<std::uint32_t>(
        Pelican::RenderGraphTransform::Status::unavailable) == 10u);
static_assert(
    static_cast<std::uint32_t>(
        Pelican::RenderStrategy::Status::invalid_argument) == 1u);
static_assert(
    static_cast<std::uint32_t>(Pelican::RenderStrategy::Status::unavailable)
    == 10u);
static_assert(
    static_cast<std::uint32_t>(
        Pelican::RenderSubgraph::Status::invalid_argument) == 1u);
static_assert(
    static_cast<std::uint32_t>(Pelican::RenderSubgraph::Status::unavailable)
    == 10u);

namespace Pelican {

namespace {

// Every provider registry a game DLL can publish into, in the single order that
// activation and release both follow. A new registry is added here once and
// reaches the initial load, the reload, the rollback and the release together.
struct GameProviderRegistry {
    void (*activate)(internal::RegistrationOwner) noexcept;
    void (*release)(internal::RegistrationOwner) noexcept;
};

constexpr GameProviderRegistry game_provider_registries[] = {
    {render_policy_internal::activateProviderOwner,
     render_policy_internal::releaseProviderOwner},
    {render_pass_internal::activateProviderOwner,
     render_pass_internal::releaseProviderOwner},
    {render_subgraph_internal::activateProviderOwner,
     render_subgraph_internal::releaseProviderOwner},
    {render_graph_transform_internal::activateProviderOwner,
     render_graph_transform_internal::releaseProviderOwner},
    {render_strategy_internal::activateProviderOwner,
     render_strategy_internal::releaseProviderOwner},
    {vulkan_native_scope_internal::activateProviderOwner,
     vulkan_native_scope_internal::releaseProviderOwner},
#if PELICAN_WITH_PHYSICS
    {physics_internal::activateProviderOwner,
     physics_internal::releaseProviderOwner},
#endif
};

using GameProviderRegistryAction =
    void (*GameProviderRegistry::*)(internal::RegistrationOwner) noexcept;

// `enabled` carries the rollback path's per-call guard: rollback re-activates
// the owner only when it actually loaded a second copy of the previous DLL.
void forEachProviderRegistry(internal::RegistrationOwner owner,
                             GameProviderRegistryAction action,
                             bool enabled = true) noexcept {
    if (!enabled) return;
    for (const auto &registry : game_provider_registries) (registry.*action)(owner);
}

} // namespace

namespace internal {

void releaseGameLogicRegistrations(RegistrationOwner owner) noexcept {
    // Live behavior instances and their callbacks must be gone while the owner
    // DLL is still loaded. Registry entries are erased only afterwards.
    releaseBehaviorOwner(owner);
#if PELICAN_WITH_ANIMATION
    Animation::releaseAnimationOwner(owner);
#endif
    forEachProviderRegistry(owner, &GameProviderRegistry::release);
    unregisterGameSystems(owner);
    unregisterBehaviors(owner);
    unregisterEvents(owner);
    unregisterComponents(owner);
    releaseRegistrationOwner(owner);
}

} // namespace internal

namespace {

std::atomic_bool reload_in_progress{false};

class ReloadStateGuard {
  public:
    ReloadStateGuard() { reload_in_progress.store(true, std::memory_order_release); }
    ~ReloadStateGuard() { reload_in_progress.store(false, std::memory_order_release); }
};

struct PlatformLoadError {
    std::uint32_t code = 0;
    std::string message;
};

PlatformLoadError platformLoadError() {
#ifdef _WIN32
    const auto code = GetLastError();
    // System messages follow the machine locale, while JSON-RPC is UTF-8.
    return {
        .code = code,
        .message = "Windows loader error code " + std::to_string(code),
    };
#else
    const char *message = dlerror();
    return {
        .message = message != nullptr ? message
                                      : "unknown dynamic-loader error",
    };
#endif
}

std::uint64_t currentProcessId() noexcept {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

#ifdef _WIN32
std::string currentExecutableName() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size()) {
        return {};
    }
    buffer.resize(length);
    return std::filesystem::path{buffer}.filename().string();
}

class SourceReadLease {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DWORD error_ = ERROR_SUCCESS;

  public:
    SourceReadLease() = default;
    SourceReadLease(const SourceReadLease &) = delete;
    SourceReadLease &operator=(const SourceReadLease &) = delete;
    ~SourceReadLease() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    bool acquire(const std::filesystem::path &path) noexcept {
        handle_ = CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            error_ = GetLastError();
            return false;
        }
        error_ = ERROR_SUCCESS;
        return true;
    }

    DWORD error() const noexcept { return error_; }
};

bool sourceWriterIsActive(DWORD error) noexcept {
    return error == ERROR_SHARING_VIOLATION ||
           error == ERROR_LOCK_VIOLATION;
}
#endif

constexpr std::uint32_t sourceBusyRetryLimit = 3;

bool processIsAlive(std::uint64_t pid) noexcept {
#ifdef _WIN32
    const auto handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    static_cast<DWORD>(pid));
    if (handle == nullptr) {
        // A live process of another user answers with access denied, exactly
        // like EPERM on POSIX.  Only a missing process is safe to sweep.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    CloseHandle(handle);
    return true;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string_view registrationKindName(internal::RegistrationKind kind) {
    switch (kind) {
    case internal::RegistrationKind::system:
        return "system";
    case internal::RegistrationKind::component:
        return "component";
    case internal::RegistrationKind::event:
        return "event";
    case internal::RegistrationKind::behavior:
        return "behavior";
    }
    return "unknown";
}

std::optional<GameLogicErrorInfo>
validateRegistrationKeys(internal::RegistrationOwner owner,
                         const std::filesystem::path &source_path) {
    auto records = internal::registrationRecords(owner);
    std::ranges::sort(records, [](const auto &left, const auto &right) {
        if (left.kind != right.kind) {
            return static_cast<std::uint8_t>(left.kind) <
                   static_cast<std::uint8_t>(right.kind);
        }
        return left.name < right.name;
    });
    for (std::size_t index = 1; index < records.size(); ++index) {
        if (records[index] != records[index - 1]) continue;
        const auto kind = registrationKindName(records[index].kind);
        nlohmann::ordered_json fields{
            {"kind", kind},
            {"key", records[index].name},
        };
        return GameLogicErrorInfo{
            .message = "game logic DLL '" + source_path.string() +
                       "' registration rejected: duplicate " +
                       std::string{kind} + " key '" + records[index].name +
                       "'",
            .code = std::string{internal::gameLogicRegistrationRejectedErrorCode},
            .fields = std::move(fields),
        };
    }
    return std::nullopt;
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

void GameLogicReloader::setLastError(GameLogicErrorInfo error) {
    last_error = std::move(error.message);
    last_error_code = std::move(error.code);
    last_error_fields = std::move(error.fields);
    if (!last_error_fields.is_object()) {
        last_error_fields = nlohmann::ordered_json::object();
    }
}

void GameLogicReloader::clearLastError() noexcept {
    last_error.clear();
    last_error_code.clear();
    last_error_fields = nlohmann::ordered_json::object();
}

void GameLogicReloader::clearSourceBusy() noexcept {
    source_busy_retries = 0;
    source_busy_pending = false;
    if (last_error_code == internal::gameLogicSourceBusyErrorCode) {
        clearLastError();
    }
}

void GameLogicReloader::sweepStaleShadowDirectories() noexcept {
    if (shadow_directory.empty()) return;
    const auto root = shadow_directory.parent_path();
    std::error_code ec;
    std::filesystem::directory_iterator entries{root, ec};
    if (ec) return; // The directory is created lazily by makeShadowCopy().
    for (const auto &entry : entries) {
        if (!entry.is_directory(ec)) {
            ec.clear();
            continue;
        }
        const auto name = entry.path().filename().string();
        std::uint64_t pid = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), pid);
        if (name.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != name.data() + name.size() || processIsAlive(pid)) {
            continue;
        }
        std::filesystem::remove_all(entry.path(), ec);
        if (ec && logger != nullptr) {
            LOG_WARNING(logger, "cannot remove stale game logic shadow directory '{}': {}",
                        entry.path().string(), ec.message());
        }
        ec.clear();
    }
}

std::optional<GameLogicReloader::SourceFingerprint>
GameLogicReloader::sourceFingerprint(
    const std::filesystem::path &path, std::error_code &error) const noexcept {
    error.clear();
    const auto write_time = std::filesystem::last_write_time(path, error);
    if (error) return std::nullopt;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return std::nullopt;
    return SourceFingerprint{.write_time = write_time, .size = size};
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
                           GameLogicErrorInfo &error) {
#ifdef _WIN32
    const auto diagnostic = internal::diagnoseGameLogicImports(
        path, currentExecutableName(), gameLogicAbiVersion);
    if (!diagnostic.accepted()) {
        internal::releaseGameLogicRegistrations(owner);
        error = {
            .message = "game logic DLL '" + source_path.string() +
                       "' rejected before load: " +
                       diagnostic.error_code +
                       diagnostic.error_fields.dump(),
            .code = diagnostic.error_code,
            .fields = diagnostic.error_fields,
        };
        return std::nullopt;
    }
#endif
    internal::ScopedRegistrationOwner registration_scope{owner};
#ifdef _WIN32
    const auto handle = LoadLibraryW(path.c_str());
#else
    const auto handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (handle == nullptr) {
        const auto platform_error = platformLoadError();
        internal::releaseGameLogicRegistrations(owner);
        error = {
            .message = "game logic DLL '" + source_path.string() +
                       "' load failed: " + platform_error.message,
            .code = std::string{internal::gameLogicLoadFailedErrorCode},
            .fields = {{"os_code", platform_error.code}},
        };
        return std::nullopt;
    }

    if (auto validation = validateRegistrationKeys(owner, source_path)) {
        error = std::move(*validation);
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

bool GameLogicReloader::validateCandidate(const std::filesystem::path &path,
                                          GameLogicErrorInfo &error) {
    const auto owner = internal::allocateRegistrationOwner();
    auto candidate = loadCopy(path, owner, error);
    if (!candidate) return false;
    try {
        if (active) {
            std::optional<std::span<const BehaviorReloadSourceParams>>
                behavior_sources;
            if (const auto *config =
                    FastModuleContainer::tryGet<ProjectBasicConfig>()) {
                behavior_sources =
                    config->resolvedScene().behaviorReloadSources();
            }
            internal::validateBehaviorReloadSources(
                active->owner, candidate->owner, behavior_sources);
        }
    } catch (const std::exception &validation_error) {
        error = {
            .message = validation_error.what(),
            .code =
                std::string{internal::gameLogicRegistrationRejectedErrorCode},
            .fields = {{"reason", validation_error.what()}},
        };
        unload(*candidate);
        return false;
    }
    unload(*candidate);
    return true;
}

bool GameLogicReloader::initialize(const std::filesystem::path &source) {
    const auto requested_source =
        std::filesystem::absolute(source).lexically_normal();
    const auto requested_shadow =
        requested_source.parent_path() / ".pelican-hot-reload" /
        std::to_string(currentProcessId());
    const auto shadow_before_sweep = shadow_directory;
    shadow_directory = requested_shadow;
    sweepStaleShadowDirectories();
    std::error_code ec;
    const auto requested_fingerprint = sourceFingerprint(requested_source, ec);
    if (!requested_fingerprint) {
        if (!active) {
            source_path = requested_source;
            shadow_directory = requested_shadow;
        } else {
            shadow_directory = shadow_before_sweep;
        }
        setLastError({
            .message = "game logic DLL '" + requested_source.string() +
                       "' is unavailable: " + ec.message(),
            .code = std::string{internal::gameLogicUnavailableErrorCode},
            .fields = {{"reason", ec.message()}},
        });
        LOG_ERROR(logger, "{}", last_error);
        return false;
    }

    if (active) {
        const auto previous_source = source_path;
        const auto previous_shadow = shadow_directory;
        const auto previous_observed_source = observed_source;
        const auto previous_settling_source = settling_source;
        const auto previous_copy_sequence = copy_sequence;
        source_path = requested_source;
        shadow_directory = requested_shadow;
        observed_source = *requested_fingerprint;
        settling_source.reset();
        const bool committed = reloadTransaction([] {}, [] {});
        if (!committed) {
            source_path = previous_source;
            shadow_directory = previous_shadow;
            observed_source = previous_observed_source;
            settling_source = previous_settling_source;
            copy_sequence = previous_copy_sequence;
        }
        return committed;
    }

    // Even with no active DLL, preflight must occur before shutdown/reset so
    // an incompatible image is never executed as a side effect of initialize.
    source_path = requested_source;
    shadow_directory = requested_shadow;
    observed_source = *requested_fingerprint;
    settling_source.reset();
    copy_sequence = 0;
    clearSourceBusy();

    try {
        const auto copy = makeShadowCopy("initial");
        GameLogicErrorInfo error;
        const auto owner = internal::allocateRegistrationOwner();
        active = loadCopy(copy, owner, error);
        if (!active) {
            removeFileNoThrow(copy);
            setLastError(std::move(error));
            LOG_ERROR(logger, "{}", last_error);
            return false;
        }
        forEachProviderRegistry(owner, &GameProviderRegistry::activate);
        generation = 1;
        clearLastError();
        LOG_INFO(logger, "game logic DLL loaded: source='{}' copy='{}' epoch={} systems={}",
                 source_path.string(), copy.string(), gameLogicAbiVersion,
                 internal::gameSystemRegistrationCount(owner));
        return true;
    } catch (const std::exception &error) {
        setLastError({
            .message = error.what(),
            .code = std::string{internal::gameLogicShadowCopyFailedErrorCode},
            .fields = {{"reason", error.what()}},
        });
        LOG_ERROR(logger, "{}", last_error);
        return false;
    }
}

bool GameLogicReloader::reloadTransaction(const ResetFn &teardown, const ResetFn &rebuild) {
    std::filesystem::path candidate_path;
    try {
        candidate_path = makeShadowCopy("candidate");
    } catch (const std::exception &error) {
        setLastError({
            .message = error.what(),
            .code = std::string{internal::gameLogicShadowCopyFailedErrorCode},
            .fields = {{"reason", error.what()}},
        });
        LOG_ERROR(logger, "{}; keeping previous game logic DLL", last_error);
        return false;
    }

    GameLogicErrorInfo validation_error;
    if (!validateCandidate(candidate_path, validation_error)) {
        removeFileNoThrow(candidate_path);
        setLastError(std::move(validation_error));
        LOG_ERROR(logger, "{}; keeping previous game logic DLL", last_error);
        return false;
    }

    ReloadStateGuard reload_state;
    auto previous = std::move(active);
    bool previous_unloaded = false;
    active.reset();
    std::string reload_error;
    GameLogicErrorInfo transaction_error;
    try {
        teardown();
        if (previous) {
            unload(*previous);
            previous_unloaded = true;
        }

        GameLogicErrorInfo load_error;
        const auto owner = internal::allocateRegistrationOwner();
        active = loadCopy(candidate_path, owner, load_error);
        if (!active) {
            transaction_error = std::move(load_error);
            throw std::runtime_error(transaction_error.message);
        }
        forEachProviderRegistry(owner, &GameProviderRegistry::activate);
        rebuild();

        ++generation;
        clearLastError();
        if (previous) removeFileNoThrow(previous->path);
        LOG_INFO(logger, "game logic DLL reloaded: source='{}' generation={} systems={}",
                 source_path.string(), generation, internal::gameSystemRegistrationCount(owner));
        return true;
    } catch (const std::exception &error) {
        // Copy the message and leave the handler before unloading a DLL that
        // may own the active exception's destructor/unwind metadata.
        reload_error = error.what();
        if (transaction_error.code.empty()) {
            transaction_error = {
                .message = reload_error,
                .code = std::string{internal::gameLogicReloadFailedErrorCode},
                .fields = {{"reason", reload_error}},
            };
        }
    } catch (...) {
        reload_error = "non-standard exception";
        transaction_error = {
            .message = reload_error,
            .code = std::string{internal::gameLogicReloadFailedErrorCode},
            .fields = {{"reason", reload_error}},
        };
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
            GameLogicErrorInfo rollback_load_error;
            active = loadCopy(previous->path, rollback_owner,
                              rollback_load_error);
            rollback_error = rollback_load_error.message;
            activate_rollback_owner = active.has_value();
        } else {
            // Teardown failed before the old DLL was unloaded. Reuse that
            // exact handle/owner instead of loading a second copy whose
            // static registrations would not have a well-defined owner.
            active = std::move(previous);
        }
        if (active) {
            forEachProviderRegistry(active->owner, &GameProviderRegistry::activate,
                                    activate_rollback_owner);
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
    transaction_error.message =
        "game logic DLL '" + source_path.string() +
        "' reload failed: " + reload_error;
    if (!rollback_error.empty()) {
        transaction_error.message += "; rollback failed: " + rollback_error;
        transaction_error.fields["rollback_error"] = rollback_error;
    }
    setLastError(std::move(transaction_error));
    LOG_ERROR(logger, "{}", last_error);
    return false;
}

GameLogicReloadAttempt GameLogicReloader::reloadNowAttempt(const ResetFn &teardown,
                                                           const ResetFn &rebuild) {
    if (source_path.empty()) {
        setLastError({
            .message =
                "game logic DLL reload requested, but no DLL is configured",
            .code = std::string{internal::gameLogicNotConfiguredErrorCode},
        });
        return {.attempted = true,
                .error = last_error,
                .error_code = last_error_code,
                .error_fields = last_error_fields};
    }
    // A forced/manual reload acknowledges the current source version too. This
    // prevents the next automatic poll from applying the same DLL a second time.
    std::error_code ec;
    if (const auto fingerprint = sourceFingerprint(source_path, ec)) {
        observed_source = *fingerprint;
        settling_source.reset();
    }

    const bool committed = reloadTransaction(teardown, rebuild);
    return {
        .attempted = true,
        .committed = committed,
        .error = committed ? std::string{} : last_error,
        .error_code = committed ? std::string{} : last_error_code,
        .error_fields = committed ? nlohmann::ordered_json::object()
                                  : last_error_fields,
    };
}

GameLogicReloadAttempt GameLogicReloader::pollAttempt(const ResetFn &teardown,
                                                      const ResetFn &rebuild, bool force) {
    if (source_path.empty()) return {};
    std::error_code ec;
    const auto fingerprint = sourceFingerprint(source_path, ec);
    if (!fingerprint) {
        clearSourceBusy();
        setLastError({
            .message = "game logic DLL '" + source_path.string() +
                       "' stat failed: " + ec.message(),
            .code = std::string{internal::gameLogicUnavailableErrorCode},
            .fields = {{"reason", ec.message()}},
        });
        return {.attempted = true,
                .error = last_error,
                .error_code = last_error_code,
                .error_fields = last_error_fields};
    }

#ifdef _WIN32
    SourceReadLease source_lease;
#endif
    if (!force) {
        if (*fingerprint == observed_source) {
            settling_source.reset();
            return {};
        }
        if (!settling_source || *settling_source != *fingerprint) {
            settling_source = *fingerprint;
            return {};
        }
#ifdef _WIN32
        // A build can truncate and refill the DLL in place. Two matching
        // metadata samples debounce changing sizes/timestamps; this read lease
        // then excludes writers while the shadow copy is taken. A busy source
        // is not a failed reload and remains pending for the next poll.
        if (!source_lease.acquire(source_path)) {
            if (sourceWriterIsActive(source_lease.error())) {
                source_busy_pending = true;
                ++source_busy_retries;
                if (source_busy_retries >= sourceBusyRetryLimit) {
                    setLastError({
                        .message = "game logic DLL '" + source_path.string() +
                                   "' remains busy after " +
                                   std::to_string(source_busy_retries) + " polls",
                        .code = std::string{internal::gameLogicSourceBusyErrorCode},
                        .fields = {{"path", source_path.generic_string()},
                                   {"retries", source_busy_retries}},
                    });
                    return {.attempted = true,
                            .error = last_error,
                            .error_code = last_error_code,
                            .error_fields = last_error_fields};
                }
                return {};
            }
            clearSourceBusy();
            setLastError({
                .message = "game logic DLL '" + source_path.string() +
                           "' readiness open failed: Windows error code " +
                           std::to_string(source_lease.error()),
                .code = std::string{internal::gameLogicUnavailableErrorCode},
                .fields = {{"reason", "readiness_open_failed"},
                           {"os_code", source_lease.error()}},
            });
            return {.attempted = true,
                    .error = last_error,
                    .error_code = last_error_code,
                    .error_fields = last_error_fields};
        }
        std::error_code locked_ec;
        const auto locked_fingerprint =
            sourceFingerprint(source_path, locked_ec);
        if (!locked_fingerprint || *locked_fingerprint != *fingerprint) {
            if (locked_fingerprint) settling_source = *locked_fingerprint;
            return {};
        }
        clearSourceBusy();
#endif
    }
    observed_source = *fingerprint;
    settling_source.reset();
    const bool committed = reloadTransaction(teardown, rebuild);
    return {
        .attempted = true,
        .committed = committed,
        .error = committed ? std::string{} : last_error,
        .error_code = committed ? std::string{} : last_error_code,
        .error_fields = committed ? nlohmann::ordered_json::object()
                                  : last_error_fields,
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
    const auto own_shadow_directory = shadow_directory;
    source_path.clear();
    shadow_directory.clear();
    observed_source = {};
    settling_source.reset();
    clearLastError();
    source_busy_retries = 0;
    source_busy_pending = false;
    generation = 0;
    if (!own_shadow_directory.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(own_shadow_directory, ec);
        if (ec && logger != nullptr) {
            LOG_WARNING(logger, "cannot remove game logic shadow directory '{}': {}",
                        own_shadow_directory.string(), ec.message());
        }
    }
}

GameLogicReloadStatus GameLogicReloader::status() const {
    const auto owner = active ? active->owner : internal::engineRegistrationOwner;
    return GameLogicReloadStatus{
        .configured = !source_path.empty(),
        .loaded = active.has_value(),
        .reloading = isGameLogicReloadInProgress(),
        .pending = source_busy_pending,
        .source_busy_retries = source_busy_retries,
        .generation = generation,
        .system_count = active ? internal::gameSystemRegistrationCount(owner) : 0,
        .source = source_path,
        .loaded_copy = active ? active->path : std::filesystem::path{},
        .last_error = last_error,
        .error_code = last_error_code,
        .error_fields = last_error_fields,
    };
}

bool isGameLogicReloadInProgress() noexcept {
    return reload_in_progress.load(std::memory_order_acquire);
}

namespace {
void runtimeTeardown() {
    RuntimeTeardownGuard guard;
    requireRuntimeTeardownSuccess(guard.run());
}

void rebuildCurrentScene() {
    auto &scene_loader = GET_MODULE(SceneLoader);
    auto scene = scene_loader.currentScene();
    if (scene.empty()) return;
    GET_MODULE(ProjectBasicConfig).refreshResolvedScene(
        [&scene_loader, scene = std::move(scene)]() mutable {
            auto &config = GET_MODULE(ProjectBasicConfig);
            auto request = SceneLoadRequest::borrowed(
                std::move(scene), config.sceneProjectionState());
            (void)requireSceneLoadCommitted(
                scene_loader.load(std::move(request)));
        });
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
