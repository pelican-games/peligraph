#pragma once

#include "../container.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace Pelican {

struct GameLogicReloadStatus {
    bool configured = false;
    bool loaded = false;
    bool reloading = false;
    std::uint64_t generation = 0;
    std::size_t system_count = 0;
    std::filesystem::path source;
    std::filesystem::path loaded_copy;
    std::string last_error;
    std::string error_code;
    nlohmann::ordered_json error_fields = nlohmann::ordered_json::object();
};

struct GameLogicReloadAttempt {
    bool attempted = false;
    bool committed = false;
    std::string error;
    std::string error_code;
    nlohmann::ordered_json error_fields = nlohmann::ordered_json::object();
};

struct GameLogicErrorInfo {
    std::string message;
    std::string code;
    nlohmann::ordered_json fields = nlohmann::ordered_json::object();
};

namespace internal {
void releaseGameLogicRegistrations(RegistrationOwner owner) noexcept;
}

DECLARE_MODULE(GameLogicReloader) {
    using ResetFn = std::function<void()>;

    struct LoadedLibrary {
        void *handle = nullptr;
        std::filesystem::path path;
        internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    };
    struct SourceFingerprint {
        std::filesystem::file_time_type write_time{};
        std::uintmax_t size = 0;

        bool operator==(const SourceFingerprint &) const = default;
    };
    std::optional<LoadedLibrary> active;
    std::filesystem::path source_path;
    std::filesystem::path shadow_directory;
    SourceFingerprint observed_source;
    std::optional<SourceFingerprint> settling_source;
    std::uint64_t copy_sequence = 0;
    std::uint64_t generation = 0;
    std::string last_error;
    std::string last_error_code;
    nlohmann::ordered_json last_error_fields = nlohmann::ordered_json::object();

    std::optional<SourceFingerprint> sourceFingerprint(
        const std::filesystem::path &path, std::error_code &error) const noexcept;
    std::filesystem::path makeShadowCopy(const char *purpose);
    std::optional<LoadedLibrary> loadCopy(const std::filesystem::path &path,
                                          internal::RegistrationOwner owner,
                                          GameLogicErrorInfo &error);
    void unload(LoadedLibrary &library) noexcept;
    bool validateCandidate(const std::filesystem::path &path,
                           GameLogicErrorInfo &error);
    bool reloadTransaction(const ResetFn &teardown, const ResetFn &rebuild);
    void setLastError(GameLogicErrorInfo error);
    void clearLastError() noexcept;

  public:
    GameLogicReloader() = default;
    ~GameLogicReloader();

    bool initialize(const std::filesystem::path &source);
    GameLogicReloadAttempt reloadNowAttempt(const ResetFn &teardown, const ResetFn &rebuild);
    GameLogicReloadAttempt pollAttempt(const ResetFn &teardown, const ResetFn &rebuild,
                                       bool force = false);
    bool reloadNow(const ResetFn &teardown, const ResetFn &rebuild);
    bool poll(const ResetFn &teardown, const ResetFn &rebuild, bool force = false);
    void shutdown() noexcept;
    GameLogicReloadStatus status() const;
};

bool isGameLogicReloadInProgress() noexcept;
bool initializeConfiguredGameLogic();
GameLogicReloadAttempt reloadConfiguredGameLogicAttempt(bool force = true);
GameLogicReloadAttempt pollConfiguredGameLogicAttempt(bool force = false);
bool reloadConfiguredGameLogic(bool force = true);
void pollConfiguredGameLogic(bool force = false);
void shutdownConfiguredGameLogic() noexcept;
GameLogicReloadStatus configuredGameLogicStatus();

} // namespace Pelican
