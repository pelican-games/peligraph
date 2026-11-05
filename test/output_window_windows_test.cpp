#include "../src/project/projecttemplate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

std::wstring quoteArgument(const std::wstring &argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

class Player {
  public:
    Player() {
        root_ = fs::temp_directory_path() /
                ("pelican_md1a1_windows_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        (void)Pelican::initializeProjectTemplate(root_, Pelican::ProjectTemplate::Blank);
        log_ = root_ / "player.log";
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        REQUIRE(::CreatePipe(&stdin_read_, &stdin_write_, &attributes, 0));
        REQUIRE(::CreatePipe(&stdout_read_, &stdout_write_, &attributes, 0));
        REQUIRE(::SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0));
        REQUIRE(::SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0));
        log_handle_ = ::CreateFileW(log_.wstring().c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(log_handle_ != INVALID_HANDLE_VALUE);

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read_;
        startup.hStdOutput = stdout_write_;
        startup.hStdError = log_handle_;
        const std::vector<std::string> arguments{
            PELICAN_MD1A_PLAYER, "--project", root_.string(),
            "--user-dir", (root_ / "user").string(), "--rpc",
            "--size", "160x120", "--frames", "300",
            "--output-window", "0", "--output-window-size", "128x96"};
        std::wstring command;
        for (const auto &argument : arguments) {
            if (!command.empty()) command.push_back(L' ');
            command += quoteArgument(fs::path{argument}.wstring());
        }
        REQUIRE(::CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process_));
        ::CloseHandle(stdin_read_);
        stdin_read_ = INVALID_HANDLE_VALUE;
        ::CloseHandle(stdout_write_);
        stdout_write_ = INVALID_HANDLE_VALUE;
        reader_ = std::thread([this] { readLoop(); });
    }

    ~Player() {
        close();
        std::error_code ignored;
        fs::remove_all(root_, ignored);
    }

    Json request(std::string_view method) {
        const int id = next_id_++;
        const Json message{{"jsonrpc", "2.0"}, {"id", id},
                           {"method", std::string{method}}, {"params", Json::object()}};
        const auto line = message.dump() + "\n";
        std::size_t offset = 0;
        while (offset < line.size()) {
            DWORD written = 0;
            REQUIRE(::WriteFile(stdin_write_, line.data() + offset,
                                static_cast<DWORD>(line.size() - offset), &written, nullptr));
            REQUIRE(written != 0);
            offset += written;
        }
        std::unique_lock lock{mutex_};
        REQUIRE(arrived_.wait_for(lock, std::chrono::seconds{60}, [this] {
            return !lines_.empty() || closed_;
        }));
        REQUIRE_FALSE(lines_.empty());
        const auto response = Json::parse(lines_.front());
        lines_.pop_front();
        REQUIRE(response.at("id") == id);
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

    void close() {
        if (stdin_write_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(stdin_write_);
            stdin_write_ = INVALID_HANDLE_VALUE;
        }
        if (process_.hProcess != nullptr) {
            if (::WaitForSingleObject(process_.hProcess, 30000) == WAIT_TIMEOUT) {
                (void)::TerminateProcess(process_.hProcess, 1);
                (void)::WaitForSingleObject(process_.hProcess, 5000);
            }
            DWORD code = 0;
            if (::GetExitCodeProcess(process_.hProcess, &code)) exit_code_ = static_cast<int>(code);
        }
        if (reader_.joinable()) reader_.join();
        if (stdout_read_ != INVALID_HANDLE_VALUE) ::CloseHandle(stdout_read_);
        if (process_.hThread != nullptr) ::CloseHandle(process_.hThread);
        if (process_.hProcess != nullptr) ::CloseHandle(process_.hProcess);
        if (log_handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(log_handle_);
        stdout_read_ = INVALID_HANDLE_VALUE;
        process_ = {};
        log_handle_ = INVALID_HANDLE_VALUE;
    }

    int exitCode() const { return exit_code_; }

  private:
    void readLoop() {
        std::string pending;
        char buffer[4096];
        for (;;) {
            DWORD count = 0;
            if (!::ReadFile(stdout_read_, buffer, sizeof buffer, &count, nullptr) || count == 0) break;
            pending.append(buffer, count);
            std::size_t newline = 0;
            while ((newline = pending.find('\n')) != std::string::npos) {
                auto line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) {
                    std::lock_guard lock{mutex_};
                    lines_.push_back(std::move(line));
                }
                arrived_.notify_all();
            }
        }
        std::lock_guard lock{mutex_};
        closed_ = true;
        arrived_.notify_all();
    }

    fs::path root_;
    fs::path log_;
    HANDLE stdin_read_ = INVALID_HANDLE_VALUE;
    HANDLE stdin_write_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_read_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_write_ = INVALID_HANDLE_VALUE;
    HANDLE log_handle_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process_{};
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<std::string> lines_;
    bool closed_ = false;
    int next_id_ = 1;
    int exit_code_ = -1;
};

} // namespace

TEST_CASE("MD-1a1 Windows output window sized smoke", "[md1a1]") {
    Player player;
    Json status;
    for (int attempt = 0; attempt < 100; ++attempt) {
        status = player.request("get_status");
        const auto &output = status.at("output_window");
        if (output.at("state") == std::string{"running"} &&
            output.at("captures").get<std::uint64_t>() > 0 &&
            output.at("presented").get<std::uint64_t>() > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    const auto &output = status.at("output_window");
    CHECK(output.at("state") == std::string{"running"});
    CHECK(output.at("source") == std::string{"display"});
    CHECK(output.at("width") == 128);
    CHECK(output.at("height") == 96);
    CHECK(output.at("captures").get<std::uint64_t>() > 0);
    CHECK(output.at("presented").get<std::uint64_t>() > 0);
    CHECK(output.at("last_error").is_null());
    player.close();
    CHECK(player.exitCode() == 0);
}
