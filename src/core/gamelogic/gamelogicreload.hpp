#pragma once

#include "../container.hpp"
#include "../userpublic/details/reload/registrationowner.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
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
};

DECLARE_MODULE(GameLogicReloader) {
    using ResetFn = std::function<void()>;

    struct LoadedLibrary {
        void *handle = nullptr;
        std::filesystem::path path;
        internal::RegistrationOwner owner = internal::engineRegistrationOwner;
    };
    std::optional<LoadedLibrary> active;
    std::filesystem::path source_path;
    std::filesystem::path shadow_directory;
    std::filesystem::file_time_type observed_write_time{};
    std::uint64_t copy_sequence = 0;
    std::uint64_t generation = 0;
    std::string last_error;

    std::filesystem::path makeShadowCopy(const char *purpose);
    std::optional<LoadedLibrary> loadCopy(const std::filesystem::path &path,
                                          internal::RegistrationOwner owner,
                                          std::string &error);
    void unload(LoadedLibrary &library) noexcept;
    bool validateCandidate(const std::filesystem::path &path, std::string &error);
    bool reloadTransaction(const ResetFn &teardown, const ResetFn &rebuild);

  public:
    GameLogicReloader() = default;
    ~GameLogicReloader();

    bool initialize(const std::filesystem::path &source);
    bool reloadNow(const ResetFn &teardown, const ResetFn &rebuild);
    bool poll(const ResetFn &teardown, const ResetFn &rebuild, bool force = false);
    void shutdown() noexcept;
    GameLogicReloadStatus status() const;
};

bool isGameLogicReloadInProgress() noexcept;
bool initializeConfiguredGameLogic();
bool reloadConfiguredGameLogic(bool force = true);
void pollConfiguredGameLogic(bool force = false);
void shutdownConfiguredGameLogic() noexcept;
GameLogicReloadStatus configuredGameLogicStatus();

} // namespace Pelican
