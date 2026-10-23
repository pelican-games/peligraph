// PJ-1: edit_project_settings through the production RPC path: a headless
// `pelican_player --rpc` child, driven line by line over its stdin/stdout.

#include "../src/project/assetsmanifest.hpp"
#include "../src/project/projectdocumenttransaction.hpp"
#include "../src/project/projecttemplate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string readFile(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::string digestOf(const std::filesystem::path &path) { return Pelican::projectDocumentDigest(readFile(path)); }

struct TempProject {
    std::filesystem::path root;

    explicit TempProject(std::string_view name) {
        root = std::filesystem::temp_directory_path() /
               ("pelican_pj1_" + std::string{name} + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        (void)Pelican::initializeProjectTemplate(root, Pelican::ProjectTemplate::Blank);
        std::filesystem::copy_file(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" / "ground.glb",
                                   root / "assets" / "models" / "ground.glb");
    }
    ~TempProject() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path projectJson() const { return root / "project.json"; }
};

#ifdef _WIN32
// One headless player with --rpc. Requests are written one line at a time and
// each answer is read before the next request goes out.
class PlayerRpc {
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    PROCESS_INFORMATION process_{};
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable arrived_;
    std::deque<std::string> lines_;
    bool closed_ = false;
    int next_id_ = 1;

    void readLoop() {
        std::string pending;
        char buffer[4096];
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(stdout_read_, buffer, sizeof buffer, &read, nullptr) || read == 0) break;
            pending.append(buffer, read);
            for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
                auto line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::scoped_lock lock{mutex_};
                lines_.push_back(std::move(line));
                arrived_.notify_all();
            }
        }
        std::scoped_lock lock{mutex_};
        closed_ = true;
        arrived_.notify_all();
    }

  public:
    PlayerRpc(const std::filesystem::path &project, const std::filesystem::path &stderr_log) {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE stdin_read = nullptr;
        HANDLE stdout_write = nullptr;
        REQUIRE(CreatePipe(&stdin_read, &stdin_write_, &inherit, 0));
        REQUIRE(CreatePipe(&stdout_read_, &stdout_write, &inherit, 0));
        SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0);
        HANDLE error_log = CreateFileW(stderr_log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(error_log != INVALID_HANDLE_VALUE);

        const std::filesystem::path player{PELICAN_PJ1_PLAYER};
        std::wstring command = L"\"" + player.wstring() + L"\" --rpc --headless --size 160x90 --project \"" +
                               project.wstring() + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof startup;
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = stdin_read;
        startup.hStdOutput = stdout_write;
        startup.hStdError = error_log;
        const auto working = player.parent_path().wstring();
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                            nullptr, working.c_str(), &startup, &process_);
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(error_log);
        REQUIRE(started);
        reader_ = std::thread{[this] { readLoop(); }};
    }

    ~PlayerRpc() {
        if (stdin_write_ != nullptr) CloseHandle(stdin_write_);
        if (process_.hProcess != nullptr) {
            // The player leaves when its input ends; one that does not is
            // this test's own child, stopped so the run cannot hang.
            if (WaitForSingleObject(process_.hProcess, 120000) != WAIT_OBJECT_0) {
                TerminateProcess(process_.hProcess, 1);
                WaitForSingleObject(process_.hProcess, 10000);
            }
            CloseHandle(process_.hThread);
            CloseHandle(process_.hProcess);
        }
        if (reader_.joinable()) reader_.join();
        if (stdout_read_ != nullptr) CloseHandle(stdout_read_);
    }
    PlayerRpc(const PlayerRpc &) = delete;
    PlayerRpc &operator=(const PlayerRpc &) = delete;

    // The whole response object ({result} or {error}).
    Json call(const std::string &method, const Json &params = Json::object()) {
        const int id = next_id_++;
        const auto line = Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}.dump() + "\n";
        DWORD written = 0;
        REQUIRE(WriteFile(stdin_write_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr));
        REQUIRE(written == line.size());
        std::unique_lock lock{mutex_};
        const auto deadline = std::chrono::steady_clock::now() + 180s;
        for (;;) {
            while (!lines_.empty()) {
                auto response = Json::parse(lines_.front());
                lines_.pop_front();
                if (response.value("id", -1) == id) return response;
            }
            INFO("waiting for the answer to " << method);
            REQUIRE_FALSE(closed_);
            REQUIRE(arrived_.wait_until(lock, deadline) != std::cv_status::timeout);
        }
    }

    Json result(const std::string &method, const Json &params = Json::object()) {
        auto response = call(method, params);
        INFO(method << " -> " << response.dump());
        REQUIRE(response.contains("result"));
        return response.at("result");
    }

    // error.data of a refused call.
    Json refusal(const std::string &method, const Json &params) {
        auto response = call(method, params);
        INFO(method << " -> " << response.dump());
        REQUIRE(response.contains("error"));
        REQUIRE(response.at("error").at("code") == -32000);
        return response.at("error").at("data");
    }

    Json waitVerified() {
        const auto deadline = std::chrono::steady_clock::now() + 60s;
        for (;;) {
            auto status = result("get_status").at("assets_verification");
            if (status.at("state") != "running") return status;
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(50ms);
        }
    }
};
#endif

// In the order project.json has them.
std::vector<std::string> keysOf(const nlohmann::ordered_json &object) {
    std::vector<std::string> keys;
    for (const auto &[key, value] : object.items()) {
        (void)value;
        keys.push_back(key);
    }
    return keys;
}

} // namespace

#ifdef _WIN32
TEST_CASE("PJ-1 a Blank project gains a store, a watch and a verified manifest in place, and gives them back",
          "[pj1][project-settings]") {
    TempProject project{"blank"};
    const auto original = readFile(project.projectJson());
    const auto original_keys = keysOf(nlohmann::ordered_json::parse(original));
    PlayerRpc player{project.root, project.root.parent_path() / (project.root.filename().string() + ".stderr.log")};

    auto status = player.result("get_status");
    REQUIRE(status.at("stores") == Json::object());
    REQUIRE(status.at("reload").at("watch").is_null());
    REQUIRE(status.at("assets_verification").at("state") == "not_run");
    const auto before = player.result("load_gltf", {{"path", "assets/models/ground.glb"}, {"name", "before"}});

    auto settings = player.result("get_project_settings");
    REQUIRE(settings.at("project_digest") == digestOf(project.projectJson()));
    REQUIRE(settings.at("hot_reload").is_null());
    REQUIRE(settings.at("asset_stores").is_null());

    // 1. A store over assets/ and a watch, in one edit.
    auto edited = player.result("edit_project_settings",
                                {{"base_project_digest", settings.at("project_digest")},
                                 {"asset_stores", {{"main", {{"mount", "assets/"}}}}},
                                 {"hot_reload", {"assets/textures/", "./shaders"}}});
    REQUIRE(edited.at("committed") == true);
    REQUIRE(edited.at("changed") == true);
    REQUIRE(edited.at("previous_project_digest") == settings.at("project_digest"));
    REQUIRE(edited.at("project_digest") == digestOf(project.projectJson()));
    REQUIRE(edited.at("applied").at("hot_reload") == Json{"assets/textures", "shaders"});
    REQUIRE(edited.at("previous").at("asset_stores").is_null());
    REQUIRE(edited.at("asset_stores_change").at("added") == Json{"main"});
    REQUIRE(edited.at("restart_required").is_null());
    {
        const auto written = nlohmann::ordered_json::parse(readFile(project.projectJson()));
        auto expected_keys = original_keys;
        expected_keys.push_back("asset_stores");
        expected_keys.push_back("hot_reload");
        // hot_reload is set before asset_stores, so it is appended first.
        std::swap(expected_keys[expected_keys.size() - 2], expected_keys.back());
        REQUIRE(keysOf(written) == expected_keys);
        REQUIRE(written.at("asset_stores") == nlohmann::ordered_json{{"main", {{"mount", "assets/"}}}});
        REQUIRE(written.at("hot_reload") == nlohmann::ordered_json{"assets/textures", "shaders"});
        REQUIRE(written.at("basic_config") == nlohmann::ordered_json::parse(original).at("basic_config"));
    }
    status = player.result("get_status");
    REQUIRE(status.at("stores").contains("main"));
    REQUIRE(std::filesystem::path{status.at("stores").at("main").get<std::string>()} ==
            std::filesystem::canonical(project.root / "assets"));
    REQUIRE(status.at("reload").at("watch") == Json{"assets/textures", "shaders"});
    // The same reference resolves to the same file through the store.
    const auto after = player.result("load_gltf", {{"path", "assets/models/ground.glb"}, {"name", "after"}});
    REQUIRE(after.at("path") == before.at("path"));

    // 2. A manifest declared on the store: written from the disk and verified.
    const auto manifest = project.root / "assets.manifest.json";
    REQUIRE_FALSE(std::filesystem::exists(manifest));
    edited = player.result("edit_project_settings",
                           {{"base_project_digest", edited.at("project_digest")},
                            {"asset_stores", {{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}}}}});
    REQUIRE(edited.at("asset_stores_change").at("manifests_declared") == Json{"main"});
    REQUIRE(edited.at("applied").at("hot_reload") == Json{"assets/textures", "shaders"});
    const auto &verification = edited.at("assets_verification");
    REQUIRE(verification.at("generated").size() == 1);
    REQUIRE(verification.at("generated")[0].at("store") == "main");
    REQUIRE(verification.at("generated")[0].at("files").get<int>() > 0);
    REQUIRE(verification.at("failed").empty());
    REQUIRE(std::filesystem::exists(manifest));
    const auto verified = player.waitVerified();
    REQUIRE(verified.at("state") == "verified");
    REQUIRE(verified.at("stores") == 1);
    REQUIRE(verified.at("matched").get<int>() == verification.at("generated")[0].at("files").get<int>());
    REQUIRE(verified.at("errors") == 0);
    REQUIRE(verified.at("warnings") == 0);
    const auto detail = player.result("get_assets_verification");
    REQUIRE(detail.at("stores").size() == 1);
    REQUIRE(detail.at("stores")[0].at("name") == "main");
    REQUIRE(std::filesystem::path{detail.at("stores")[0].at("manifest").get<std::string>()} ==
            std::filesystem::canonical(manifest));

    // 3. Moving the mount is refused by name, and project.json stays.
    const auto declared = readFile(project.projectJson());
    auto refused = player.refusal("edit_project_settings",
                                  {{"base_project_digest", edited.at("project_digest")},
                                   {"asset_stores", {{"main", {{"mount", "assets/models"},
                                                               {"manifest", "assets.manifest.json"}}}}}});
    REQUIRE(refused.at("error_code") == "project.settings_change_unsupported@1");
    REQUIRE(refused.at("error_fields").at("key") == "asset_stores");
    REQUIRE(refused.at("error_fields").at("store") == "main");
    REQUIRE_FALSE(refused.at("error_fields").at("reason").get<std::string>().empty());
    REQUIRE(readFile(project.projectJson()) == declared);

    // 4. A digest that is not project.json's.
    refused = player.refusal("edit_project_settings",
                             {{"base_project_digest", settings.at("project_digest")}, {"hot_reload", nullptr}});
    REQUIRE(refused.at("error_code") == "external_modification");
    REQUIRE(refused.at("error_fields").at("expected") == settings.at("project_digest"));
    REQUIRE(refused.at("error_fields").at("actual") == edited.at("project_digest"));
    REQUIRE(readFile(project.projectJson()) == declared);

    // Undo: the previous values given back. The store and its manifest were
    // added in place, so they may be taken back.
    edited = player.result("edit_project_settings", {{"base_project_digest", edited.at("project_digest")},
                                                     {"asset_stores", nullptr},
                                                     {"hot_reload", nullptr}});
    REQUIRE(edited.at("asset_stores_change").at("removed") == Json{"main"});
    REQUIRE(edited.at("restart_required").is_null());
    REQUIRE(keysOf(nlohmann::ordered_json::parse(readFile(project.projectJson()))) == original_keys);
    status = player.result("get_status");
    REQUIRE(status.at("stores") == Json::object());
    REQUIRE(status.at("reload").at("watch").is_null());
    REQUIRE(player.waitVerified().at("state") == "not_run");
    const auto again = player.result("load_gltf", {{"path", "assets/models/ground.glb"}, {"name", "again"}});
    REQUIRE(again.at("path") == before.at("path"));
}

TEST_CASE("PJ-1 what the engine opened the project with is not changed in place",
          "[pj1][project-settings]") {
    TempProject project{"opened"};
    {
        auto document = nlohmann::ordered_json::parse(readFile(project.projectJson()));
        document["asset_stores"] = {{"main", {{"mount", "assets"}, {"manifest", "assets.manifest.json"}}}};
        std::ofstream{project.projectJson(), std::ios::binary | std::ios::trunc} << document.dump(2) << "\n";
        (void)Pelican::generateAssetsManifest(project.root / "assets", project.root / "assets.manifest.json",
                                              project.root / ".pelican" / "assets-hash-cache.json", "main", true);
    }
    PlayerRpc player{project.root, project.root.parent_path() / (project.root.filename().string() + ".stderr.log")};
    const auto digest = digestOf(project.projectJson());
    const auto opened = readFile(project.projectJson());
    REQUIRE(player.result("get_project_settings").at("project_digest") == digest);

    // The startup store cannot be removed, its manifest neither replaced nor
    // removed.
    auto refused = player.refusal("edit_project_settings", {{"base_project_digest", digest}, {"asset_stores", nullptr}});
    REQUIRE(refused.at("error_code") == "project.settings_change_unsupported@1");
    REQUIRE(refused.at("error_fields").at("store") == "main");
    refused = player.refusal("edit_project_settings",
                             {{"base_project_digest", digest},
                              {"asset_stores", {{"main", {{"mount", "assets"}, {"manifest", "other.manifest.json"}}}}}});
    REQUIRE(refused.at("error_code") == "project.settings_change_unsupported@1");
    refused = player.refusal("edit_project_settings",
                             {{"base_project_digest", digest}, {"asset_stores", {{"main", {{"mount", "assets"}}}}}});
    REQUIRE(refused.at("error_code") == "project.settings_change_unsupported@1");
    // A store whose mount overlaps another fails the startup check.
    refused = player.refusal("edit_project_settings",
                             {{"base_project_digest", digest},
                              {"asset_stores", {{"main", {{"mount", "assets"}, {"manifest", "assets.manifest.json"}}},
                                                {"models", {{"mount", "assets/models"}}}}}});
    REQUIRE(refused.at("error_code") == "project.settings_invalid@1");
    REQUIRE(refused.at("error_fields").at("key") == "asset_stores");
    // hot_reload goes through the HW-1 check.
    refused = player.refusal("edit_project_settings", {{"base_project_digest", digest}, {"hot_reload", {"../out"}}});
    REQUIRE(refused.at("error_code") == "project.hot_reload_invalid@1");
    REQUIRE(refused.at("error_fields").at("key") == "hot_reload");
    REQUIRE(readFile(project.projectJson()) == opened);

    // A store added beside it is fine; "assets/" and "assets" are one mount.
    const auto edited = player.result(
        "edit_project_settings",
        {{"base_project_digest", digest},
         {"asset_stores", {{"main", {{"mount", "assets/"}, {"manifest", "assets.manifest.json"}}},
                           {"scenes", {{"mount", "scenes"}}}}}});
    REQUIRE(edited.at("asset_stores_change").at("added") == Json{"scenes"});
    REQUIRE(edited.at("asset_stores_change").at("manifests_declared").empty());
    REQUIRE(player.result("get_status").at("stores").contains("scenes"));
}
#endif
