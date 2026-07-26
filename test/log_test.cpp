#include "../src/core/config.hpp"
#include "../src/core/log.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace Pelican {
namespace {

class CurrentDirectoryGuard {
  public:
    explicit CurrentDirectoryGuard(std::filesystem::path test_directory)
        : previous_(std::filesystem::current_path()),
          test_directory_(std::move(test_directory)) {
        std::filesystem::create_directories(test_directory_ / logFileName);
        std::filesystem::current_path(test_directory_);
    }

    ~CurrentDirectoryGuard() {
        std::error_code error;
        std::filesystem::current_path(previous_, error);
        std::filesystem::remove_all(test_directory_, error);
    }

    CurrentDirectoryGuard(const CurrentDirectoryGuard &) = delete;
    CurrentDirectoryGuard &operator=(const CurrentDirectoryGuard &) = delete;

  private:
    std::filesystem::path previous_;
    std::filesystem::path test_directory_;
};

} // namespace

TEST_CASE("logger falls back to stderr when its file cannot be opened",
          "[log]") {
    const auto suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    CurrentDirectoryGuard directory{
        std::filesystem::temp_directory_path() /
        ("pelican_log_fallback_" + suffix)};

    // A directory with the requested filename makes FileSink fail on every
    // supported host without relying on platform-specific permissions.
    REQUIRE_NOTHROW(setupLogger(true));
    REQUIRE(logger != nullptr);
    LOG_INFO(logger, "logger fallback test");
}

} // namespace Pelican
