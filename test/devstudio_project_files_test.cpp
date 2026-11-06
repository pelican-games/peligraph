#include "projectfiles.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace PelicanStudio;

namespace {

struct TempFolder {
    std::filesystem::path path;
    TempFolder() {
        static std::atomic<int> sequence = 0;
        path = std::filesystem::temp_directory_path() /
               ("prj1a-" + std::to_string(
                               std::chrono::steady_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence++));
        std::filesystem::create_directories(path);
    }
    ~TempFolder() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void write(const std::filesystem::path &path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void projectJson(const std::filesystem::path &root, const std::string &prefabs = "[]",
                 const std::string &extra = {}) {
    write(root / "project.json",
          R"({"schema":"pelican.project","version":1,"name":"scan","prefabs":)" +
              prefabs + R"(,"basic_config":{)" + extra + "}}");
}

const ProjectFileEntry *entry(const ProjectListing &listing, const std::string &path) {
    const auto found = std::find_if(listing.entries.begin(), listing.entries.end(),
                                    [&](const auto &candidate) { return candidate.path == path; });
    return found == listing.entries.end() ? nullptr : &*found;
}

std::string document(const std::string &schema) {
    return R"({"schema":")" + schema + R"(","version":1})";
}

#ifdef _WIN32
bool createDirectoryJunction(const std::filesystem::path &link,
                             const std::filesystem::path &target) {
    std::wstring command =
        L"cmd.exe /d /c mklink /J \"" + link.native() + L"\" \"" + target.native() + L"\"";
    std::vector<wchar_t> writable(command.begin(), command.end());
    writable.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, writable.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, 10000);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}
#endif

} // namespace

TEST_CASE("PRJ-1a a kinds come from extensions, envelopes, then declarations", "[prj1a]") {
    TempFolder temp;
    projectJson(temp.path);
    struct Case {
        std::string name;
        std::string contents;
        ProjectFileKind kind;
        std::string label;
    };
    std::vector<Case> cases{
        {"x.vert", "", ProjectFileKind::Shader, "shader"},
        {"upper.FRAG", "", ProjectFileKind::Shader, "shader"},
        {"x.surface", "", ProjectFileKind::Shader, "shader"},
        {"x.glsl", "", ProjectFileKind::Shader, "shader"},
        {"x.png", "", ProjectFileKind::Texture, "texture"},
        {"x.psd", "", ProjectFileKind::Texture, "texture"},
        {"x.wav", "", ProjectFileKind::Sound, "sound"},
        {"x.flac", "", ProjectFileKind::Sound, "sound"},
        {"bad.gltf", "not json", ProjectFileKind::Mesh, "mesh"},
        {"x.usdz", "", ProjectFileKind::Mesh, "mesh"},
        {"x.material.json", document("pelican.material"), ProjectFileKind::Material, "material"},
        {"scene.json", document("pelican.scene"), ProjectFileKind::Scene, "scene"},
        {"unknown.json", document("example.unknown"), ProjectFileKind::Other, "other"},
        {"project-like.json", document("pelican.project"), ProjectFileKind::Other, "other"},
    };
    for (const auto &schema : {"pelican.prefab"}) {
        cases.push_back({"prefab.json", document(schema), ProjectFileKind::Prefab, "prefab (not registered)"});
    }
    for (const auto &schema : {"pelican.graph", "pelican.frame_plan",
                               "pelican.authored_render_fragment", "pelican.ray_group_layout",
                               "pelican.ray_bindings"}) {
        cases.push_back({std::string{schema} + ".json", document(schema), ProjectFileKind::Graph, "graph"});
    }
    for (const auto &schema : {"pelican.render_feature", "pelican.render_feature_overlay",
                               "pelican.render_feature_parameters"}) {
        cases.push_back({std::string{schema} + ".json", document(schema), ProjectFileKind::Feature, "feature"});
    }
    for (const auto &test : cases) write(temp.path / test.name, test.contents);
    const auto listing = scanProjectFolder(temp.path);
    REQUIRE(listing.entries.size() == cases.size() + 1);
    for (const auto &test : cases) {
        CAPTURE(test.name);
        const auto *found = entry(listing, test.name);
        REQUIRE(found != nullptr);
        CHECK(found->kind == test.kind);
        CHECK(projectFileKindLabel(*found) == test.label);
    }
}

TEST_CASE("PRJ-1a b envelope position and the per-file byte limit", "[prj1a]") {
    TempFolder temp;
    projectJson(temp.path);
    write(temp.path / "late.json",
          R"({"padding":")" + std::string(1024 * 1024, 'x') +
              R"(","schema":"pelican.material","version":1})");
    write(temp.path / "large.json", R"({"padding":")" + std::string(512, ' ') +
                                      R"(","schema":"pelican.scene","version":1})");
    projectJson(temp.path,
                R"([{"name":"large","path":"large.json"}])");
    ProjectScanOptions options;
    options.max_json_bytes = 256;
    const auto listing = scanProjectFolder(temp.path, options);
    CHECK(entry(listing, "large.json")->kind == ProjectFileKind::Prefab);
    CHECK(entry(listing, "large.json")->kind != ProjectFileKind::Scene);
    CHECK(listing.json_bytes_inspected < 512);
    write(temp.path / "over.json", R"({"padding":")" + std::string(512, 'x') +
                                     R"(","schema":"pelican.material","version":1})");
    CHECK(entry(scanProjectFolder(temp.path, options), "over.json")->kind ==
          ProjectFileKind::Other);
    TempFolder late;
    projectJson(late.path);
    write(late.path / "late.json",
          R"({"padding":")" + std::string(1024 * 1024, 'x') +
              R"(","schema":"pelican.material","version":1})");
    CHECK(entry(scanProjectFolder(late.path), "late.json")->kind == ProjectFileKind::Material);
}

TEST_CASE("PRJ-1a c unreadable and malformed documents stay visible", "[prj1a]") {
    TempFolder temp;
    write(temp.path / "empty.json", "");
    write(temp.path / "malformed.json", "{");
    write(temp.path / "array.json", "[]");
    write(temp.path / "noversion.json", R"({"schema":"pelican.scene"})");
    write(temp.path / "binary.json", std::string{"\0\1\2\xFF", 4});
    const auto listing = scanProjectFolder(temp.path);
    CHECK_FALSE(listing.root_unreadable);
    for (const auto *name : {"empty.json", "malformed.json", "array.json", "noversion.json", "binary.json"}) {
        CAPTURE(name);
        REQUIRE(entry(listing, name) != nullptr);
        CHECK(entry(listing, name)->kind == ProjectFileKind::Other);
    }
#ifndef _WIN32
    if (geteuid() == 0) {
        WARN("permission checks are not meaningful as root");
        SUCCEED();
        return;
    }
    write(temp.path / "locked.json", document("pelican.scene"));
    write(temp.path / "locked" / "child.txt", "child");
    REQUIRE(chmod((temp.path / "locked.json").c_str(), 0) == 0);
    REQUIRE(chmod((temp.path / "locked").c_str(), 0) == 0);
    const auto locked = scanProjectFolder(temp.path);
    CHECK(chmod((temp.path / "locked.json").c_str(), S_IRUSR | S_IWUSR) == 0);
    CHECK(chmod((temp.path / "locked").c_str(), S_IRUSR | S_IWUSR | S_IXUSR) == 0);
    REQUIRE(entry(locked, "locked.json") != nullptr);
    CHECK(entry(locked, "locked.json")->kind == ProjectFileKind::Other);
    const auto *folder = entry(locked, "locked");
    REQUIRE(folder != nullptr);
    CHECK(folder->unreadable);
    CHECK(entry(locked, "locked/child.txt") == nullptr);
#endif
}

TEST_CASE("PRJ-1a d prefab registration is a separate fact", "[prj1a]") {
    TempFolder temp;
    projectJson(temp.path,
                R"([{"name":"yes","path":"assets/registered.json"},
                   {"name":"broken","path":"broken.json"},
                   {"name":"missing","path":"missing.json"},
                   {"name":"japanese","path":"日本.json"}])");
    write(temp.path / "assets" / "registered.json", document("pelican.prefab"));
    write(temp.path / "unregistered.json", document("pelican.prefab"));
    write(temp.path / "broken.json", "{");
    write(temp.path / std::filesystem::path{u8"日本.json"}, document("pelican.prefab"));
    const auto listing = scanProjectFolder(temp.path);
    CHECK(entry(listing, "missing.json") == nullptr);
    CHECK(projectFileKindLabel(*entry(listing, "assets/registered.json")) == "prefab (registered)");
    CHECK(projectFileKindLabel(*entry(listing, "unregistered.json")) == "prefab (not registered)");
    CHECK(projectFileKindLabel(*entry(listing, "broken.json")) == "prefab (registered)");
    CHECK(projectFileKindLabel(*entry(listing, std::string{reinterpret_cast<const char *>(u8"日本.json")})) ==
          "prefab (registered)");
    write(temp.path / "project.json", "{");
    const auto unknown = scanProjectFolder(temp.path);
    CHECK(unknown.declarations_unread);
    CHECK(projectFileKindLabel(*entry(unknown, "assets/registered.json")) == "prefab (unknown)");
}

TEST_CASE("PRJ-1a e generated and VCS folders are skipped only where specified", "[prj1a]") {
    TempFolder temp;
    for (const auto *name : {".git", ".pelican", "build", "build-debug", "out", "output"})
        write(temp.path / name / "hidden.txt", "hidden");
    write(temp.path / "nested" / ".git" / "hidden.txt", "hidden");
    write(temp.path / ".git" / "root.txt", "hidden");
    write(temp.path / "assets" / "build" / "x.png", "visible");
    write(temp.path / ".gitignore", "visible");
    write(temp.path / ".gitkeep", "visible");
    const auto listing = scanProjectFolder(temp.path);
    CHECK(entry(listing, "assets/build/x.png") != nullptr);
    CHECK(entry(listing, ".gitignore") != nullptr);
    CHECK(entry(listing, ".gitkeep") != nullptr);
    for (const auto &candidate : listing.entries) {
        CAPTURE(candidate.path);
        CHECK(candidate.path.find("hidden.txt") == std::string::npos);
        CHECK(candidate.path.find(".git/") == std::string::npos);
    }
}

TEST_CASE("PRJ-1a f entry and examined-entry limits stop promptly", "[prj1a]") {
    TempFolder exact;
    for (int i = 0; i < 5; ++i) write(exact.path / ("f" + std::to_string(i) + ".txt"), "x");
    ProjectScanOptions options;
    options.max_entries = 5;
    auto listing = scanProjectFolder(exact.path, options);
    CHECK(listing.entries.size() == 5);
    CHECK_FALSE(listing.truncated);
    write(exact.path / "sixth.txt", "x");
    listing = scanProjectFolder(exact.path, options);
    CHECK(listing.entries.size() == 5);
    CHECK(listing.truncated);

    TempFolder deep;
    write(deep.path / "root.txt", "root");
    for (int i = 0; i < 100; ++i)
        write(deep.path / "deep" / ("f" + std::to_string(i) + ".txt"), "x");
    options.max_entries = 10000;
    CHECK(entry(scanProjectFolder(deep.path, options), "root.txt") != nullptr);
    TempFolder examined;
    for (int i = 0; i < 100; ++i) write(examined.path / ("f" + std::to_string(i)), "x");
    options.max_entries = 5;
    options.max_examined_entries = 3;
    listing = scanProjectFolder(examined.path, options);
    CHECK(listing.entries.size() <= 3);
    CHECK(listing.examined_entries == 3);
    CHECK(listing.examined_limit_reached);
    CHECK(listing.truncated);
}

TEST_CASE("PRJ-1a g links are listed and never entered or read", "[prj1a]") {
    TempFolder temp;
    TempFolder outside;
    write(outside.path / "sentinel.txt", "outside");
    write(outside.path / "sentinel.json", document("pelican.prefab"));
    std::error_code error;
    std::filesystem::create_directory_symlink(outside.path, temp.path / "folder-link", error);
#ifdef _WIN32
    if (error) {
        WARN("directory symlink creation is unavailable: " << error.message());
        SUCCEED();
        return;
    }
#else
    REQUIRE(!error);
#endif
    std::filesystem::create_symlink(outside.path / "sentinel.json", temp.path / "prefab-link.json", error);
#ifndef _WIN32
    REQUIRE(!error);
#endif
    const auto listing = scanProjectFolder(temp.path);
    for (const auto &candidate : listing.entries) CHECK(candidate.path.find("sentinel") == std::string::npos);
    for (const auto *path : {"folder-link", "prefab-link.json"}) {
        const auto *found = entry(listing, path);
        REQUIRE(found != nullptr);
        CHECK(found->link);
        CHECK(found->kind == ProjectFileKind::Other);
        CHECK(projectFileKindLabel(*found) == "link");
    }
}

#ifdef _WIN32
TEST_CASE("PRJ-1a a junction to an outside folder is listed and never entered", "[prj1a]") {
    TempFolder temp;
    TempFolder outside;
    write(outside.path / "sentinel.txt", "outside");
    write(outside.path / "project.json", document("pelican.prefab"));
    if (!createDirectoryJunction(temp.path / "junction", outside.path)) {
        WARN("junction creation is unavailable");
        SUCCEED();
        return;
    }
    std::error_code error;
    std::filesystem::create_symlink(outside.path / "project.json", temp.path / "project.json", error);
    const auto listing = scanProjectFolder(temp.path);
    CHECK(entry(listing, "junction/sentinel.txt") == nullptr);
    REQUIRE(entry(listing, "junction") != nullptr);
    CHECK(entry(listing, "junction")->link);
    CHECK(listing.declarations_unread);
}

TEST_CASE("PRJ-1a Windows reparse and cloud statuses are never opened", "[prj1a]") {
    TempFolder temp;
    TempFolder outside;
    write(outside.path / "sentinel.txt", "outside");
    std::error_code error;
    std::filesystem::create_directory_symlink(outside.path, temp.path / "reparse", error);
    if (error) {
        WARN("reparse-point creation is unavailable: " << error.message());
        SUCCEED();
        return;
    }
    const auto listing = scanProjectFolder(temp.path);
    REQUIRE(entry(listing, "reparse") != nullptr);
    CHECK(entry(listing, "reparse")->link);
    CHECK(entry(listing, "reparse/sentinel.txt") == nullptr);
    SUCCEED();
}
#endif

TEST_CASE("PRJ-1a i cancellation stops entries and JSON opens", "[prj1a]") {
    TempFolder temp;
    for (int i = 0; i < 10; ++i) write(temp.path / ("f" + std::to_string(i) + ".json"), document("pelican.scene"));
    ProjectScanOptions options;
    options.max_json_total_bytes = 1;
    int calls = 0;
    options.should_stop = [&] { return ++calls > 3; };
    const auto listing = scanProjectFolder(temp.path, options);
    CHECK(listing.cancelled);
    CHECK(listing.entries.size() <= 3);
    CHECK_FALSE(listing.json_budget_exhausted);
}

TEST_CASE("PRJ-1a j scan generations reject old projects and deliveries", "[prj1a]") {
    ProjectScanGate gate;
    gate.openProject();
    const auto first = gate.beginScan();
    CHECK(gate.isCurrent(first));
    const auto second = gate.beginScan();
    CHECK_FALSE(gate.isCurrent(first));
    CHECK(gate.isCurrent(second));
    gate.invalidate();
    CHECK_FALSE(gate.isCurrent(second));
    const auto third = gate.beginScan();
    gate.openProject();
    CHECK_FALSE(gate.isCurrent(third));
    CHECK(gate.isCurrent(gate.beginScan()));
}

TEST_CASE("PRJ-1a k filters combine and retain only necessary folders", "[prj1a]") {
    TempFolder temp;
    write(temp.path / "folder" / "nested" / "ROCK.PNG", "x");
    write(temp.path / "folder" / "sound.wav", "x");
    write(temp.path / "textures" / "readme.txt", "x");
    std::filesystem::create_directories(temp.path / "empty");
    const auto listing = scanProjectFolder(temp.path);
    const auto visible = [&](const ProjectFilter &filter, const std::string &path) {
        const auto flags = projectFilterVisible(listing, filter);
        for (std::size_t i = 0; i < listing.entries.size(); ++i)
            if (listing.entries[i].path == path) return flags[i] != 0;
        return false;
    };
    const ProjectFilter both{"rock", ProjectFileKind::Texture};
    CHECK(visible(both, "folder"));
    CHECK(visible(both, "folder/nested"));
    CHECK(visible(both, "folder/nested/ROCK.PNG"));
    CHECK_FALSE(visible(both, "folder/sound.wav"));
    CHECK_FALSE(visible(both, "textures"));
    CHECK(visible({"rock.png", ProjectFileKind::Texture}, "folder/nested/ROCK.PNG"));
    CHECK_FALSE(visible({"rock", ProjectFileKind::Sound}, "folder/nested/ROCK.PNG"));
    CHECK_FALSE(visible({"empty", ProjectFileKind::Other}, "empty"));
    CHECK(visible({"folder", std::nullopt}, "folder"));
    CHECK(visible({}, "empty"));
    CHECK_FALSE(visible({"none", std::nullopt}, "empty"));
}

namespace {

using SiblingRows = std::map<std::string, std::vector<ProjectFileEntry>>;

SiblingRows siblingRows(const ProjectListing &listing) {
    SiblingRows rows;
    for (const auto &item : listing.entries) {
        const auto parent = item.parent < 0 ? std::string{} : listing.entries[item.parent].path;
        rows[parent].push_back(item);
    }
    return rows;
}

void checkReplay(const ProjectListing &shown, const ProjectListing &next) {
    auto rows = siblingRows(shown);
    const auto edits = diffProjectListings(shown, next);
    for (const auto &edit : edits) {
        if (edit.op == ProjectTreeEdit::Op::Remove) {
            for (auto it = rows.begin(); it != rows.end();) {
                if (it->first == edit.path || it->first.starts_with(edit.path + "/"))
                    it = rows.erase(it);
                else
                    ++it;
            }
            for (auto &[parent, siblings] : rows)
                siblings.erase(std::remove_if(siblings.begin(), siblings.end(), [&](const auto &item) {
                                   return item.path == edit.path;
                               }), siblings.end());
        } else {
            auto &siblings = rows[edit.parent_path];
            if (edit.op == ProjectTreeEdit::Op::Insert) {
                REQUIRE(edit.row >= 0);
                REQUIRE(edit.row <= static_cast<int>(siblings.size()));
                siblings.insert(siblings.begin() + edit.row, edit.entry);
            } else {
                const auto found = std::find_if(siblings.begin(), siblings.end(), [&](const auto &item) {
                    return item.path == edit.path;
                });
                REQUIRE(found != siblings.end());
                *found = edit.entry;
            }
        }
    }
    const auto expected = siblingRows(next);
    for (auto it = rows.begin(); it != rows.end();) {
        if (it->second.empty() && !expected.contains(it->first))
            it = rows.erase(it);
        else
            ++it;
    }
    REQUIRE(rows.size() == expected.size());
    for (const auto &[parent, siblings] : expected) {
        CAPTURE(parent);
        REQUIRE(rows[parent].size() == siblings.size());
        for (std::size_t i = 0; i < siblings.size(); ++i) {
            CHECK(rows[parent][i].path == siblings[i].path);
            CHECK(rows[parent][i].directory == siblings[i].directory);
            CHECK(rows[parent][i].kind == siblings[i].kind);
        }
    }
}

} // namespace

TEST_CASE("PRJ-1a l diffs replay on a plain sibling model", "[prj1a]") {
    TempFolder temp;
    write(temp.path / "a.txt", "a");
    write(temp.path / "c.txt", "c");
    const auto initial = scanProjectFolder(temp.path);
    CHECK(diffProjectListings(initial, initial).empty());
    write(temp.path / "b.txt", "b");
    checkReplay(initial, scanProjectFolder(temp.path));
    const auto added = scanProjectFolder(temp.path);
    std::filesystem::remove(temp.path / "b.txt");
    checkReplay(added, scanProjectFolder(temp.path));

    TempFolder folder;
    write(folder.path / "d" / "child.txt", "x");
    const auto with_folder = scanProjectFolder(folder.path);
    std::filesystem::remove_all(folder.path / "d");
    checkReplay(with_folder, scanProjectFolder(folder.path));
    const auto edits = diffProjectListings(with_folder, scanProjectFolder(folder.path));
    REQUIRE(edits.size() == 1);
    CHECK(edits[0].op == ProjectTreeEdit::Op::Remove);

    TempFolder changing;
    write(changing.path / "x.json", document("pelican.scene"));
    const auto scene = scanProjectFolder(changing.path);
    write(changing.path / "x.json", document("pelican.material"));
    const auto material = scanProjectFolder(changing.path);
    const auto update = diffProjectListings(scene, material);
    REQUIRE(update.size() == 1);
    CHECK(update[0].op == ProjectTreeEdit::Op::Update);
    checkReplay(scene, material);
    std::filesystem::remove(changing.path / "x.json");
    write(changing.path / "x.json" / "child.txt", "x");
    checkReplay(material, scanProjectFolder(changing.path));
}

TEST_CASE("PRJ-1a m the total JSON budget leaves later files visible", "[prj1a]") {
    TempFolder temp;
    write(temp.path / "a.json", document("pelican.scene"));
    write(temp.path / "b.json", document("pelican.material"));
    ProjectScanOptions options;
    options.max_json_total_bytes = 50;
    const auto listing = scanProjectFolder(temp.path, options);
    CHECK(listing.json_budget_exhausted);
    CHECK(entry(listing, "a.json") != nullptr);
    CHECK(entry(listing, "b.json") != nullptr);
    CHECK(entry(listing, "b.json")->kind == ProjectFileKind::Other);
}

TEST_CASE("PRJ-1a n a non-ASCII path round-trips as UTF-8", "[prj1a]") {
    TempFolder temp;
    const std::string name{reinterpret_cast<const char *>(u8"テクスチャ.png")};
    const auto path = temp.path / std::filesystem::path{u8"テクスチャ.png"};
    write(path, "texture");
    const auto listing = scanProjectFolder(temp.path);
    const auto *found = entry(listing, name);
    REQUIRE(found != nullptr);
    CHECK(found->kind == ProjectFileKind::Texture);
    const auto visible = projectFilterVisible(listing, {name, std::nullopt});
    CHECK(std::count(visible.begin(), visible.end(), 1) == 1);
}
