#include "../src/devcli/processrunner.hpp"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace Pelican::DevCli {
namespace {

std::filesystem::path fixtureExecutable() { return PELICAN_PROCESS_FIXTURE_PATH; }

struct TempDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("pelican_wp173_" + std::to_string(
                                      std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { std::filesystem::create_directories(path); }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string capturedLine(std::string value) {
#ifdef _WIN32
    return value + "\r\n";
#else
    return value + "\n";
#endif
}

} // namespace

TEST_CASE("CreateProcessW starts an executable from a non-ASCII path",
          "[devcli][process][wide-path]") {
    TempDirectory temp;
    const auto unicode_directory = temp.path / std::filesystem::path{u8"日本語パス"};
    std::filesystem::create_directories(unicode_directory);
    const auto copied = unicode_directory /
                        (std::filesystem::path{u8"子プロセス"}.native() +
                         fixtureExecutable().extension().native());
    std::filesystem::copy_file(fixtureExecutable(), copied);
#ifndef _WIN32
    std::filesystem::permissions(copied, std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);
#endif

    const auto result = runProcess({.executable = copied,
                                    .arguments = {"emit", "wide argument"},
                                    .timeout = std::chrono::seconds{5},
                                    .name = "wide path fixture"});
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text == capturedLine("stdout-capture:wide argument"));
    REQUIRE(result.stderr_text == capturedLine("stderr-capture:wide argument"));
}

TEST_CASE("process runner captures both streams and saves an append-only log",
          "[devcli][process][capture]") {
    TempDirectory temp;
    const auto log_path = temp.path / "logs/process.log";
    const auto result = runProcess({.executable = fixtureExecutable(),
                                    .arguments = {"emit", "captured"},
                                    .timeout = std::chrono::seconds{5},
                                    .log_path = log_path,
                                    .name = "capture fixture"});

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text == capturedLine("stdout-capture:captured"));
    REQUIRE(result.stderr_text == capturedLine("stderr-capture:captured"));
    const auto log = readFile(log_path);
    REQUIRE(log.find("=== capture fixture ===") != std::string::npos);
    REQUIRE(log.find("[stdout]\nstdout-capture:captured") != std::string::npos);
    REQUIRE(log.find("[stderr]\nstderr-capture:captured") != std::string::npos);
}

TEST_CASE("process timeout kills the hanging child without a residual process",
          "[devcli][process][timeout]") {
    TempDirectory temp;
    const auto result = runProcess({.executable = fixtureExecutable(),
                                    .arguments = {"hang"},
                                    .timeout = std::chrono::milliseconds{250},
                                    .log_path = temp.path / "timeout.log",
                                    .name = "hanging importer"});

    REQUIRE(result.timed_out);
    REQUIRE_FALSE(result.cancelled);
    REQUIRE(result.stdout_text.find("hang-ready") != std::string::npos);
    REQUIRE_FALSE(processIsRunning(result.process_id));
    REQUIRE(readFile(temp.path / "timeout.log").find("timeout=1") != std::string::npos);
}

TEST_CASE("process API cancellation kills the hanging child without a residual process",
          "[devcli][process][cancel]") {
    std::atomic_bool cancel{false};
    std::jthread requester{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        cancel.store(true);
    }};
    const auto result = runProcess({.executable = fixtureExecutable(),
                                    .arguments = {"hang"},
                                    .timeout = std::chrono::seconds{5},
                                    .cancelled = [&] { return cancel.load(); },
                                    .name = "cancelled importer"});

    REQUIRE(result.cancelled);
    REQUIRE_FALSE(result.timed_out);
    REQUIRE_FALSE(processIsRunning(result.process_id));
}

} // namespace Pelican::DevCli
