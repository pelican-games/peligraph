#include "rulesimport.hpp"

#include "gltfsceneextract.hpp"

#include "../project/importmanifest.hpp"
#include "../project/importrules.hpp"

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace Pelican::DevCli {

namespace {

struct ProjectContext {
    std::filesystem::path root;
    std::filesystem::path imports;
};

struct SelectedInput {
    std::filesystem::path absolute;
    std::string relative;
    ImportRuleMatch match;
};

struct ToolCommand {
    std::vector<std::string> prefix;
};

std::string pathString(const std::filesystem::path &path) { return path.string(); }

std::string readFile(const std::filesystem::path &path, std::string_view context) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error("failed to open " + std::string{context} + ": " + pathString(path));
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path, std::string_view contents) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("failed to create output directory: " + pathString(path.parent_path()) +
                                 " (" + ec.message() + ")");
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file.is_open()) {
        throw std::runtime_error("failed to write output: " + pathString(path));
    }
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!file) {
        throw std::runtime_error("failed to finish output: " + pathString(path));
    }
}

std::string sha256File(const std::filesystem::path &path) {
    const auto bytes = readFile(path, "generated output");
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

std::filesystem::path canonicalPath(const std::filesystem::path &path, std::string_view context) {
    std::error_code ec;
    const auto result = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        throw std::runtime_error(std::string{context} + " path error: " + pathString(path) +
                                 " (" + ec.message() + ")");
    }
    return result;
}

ProjectContext loadProject(const std::filesystem::path &argument) {
    auto path = canonicalPath(argument.is_absolute() ? argument : std::filesystem::current_path() / argument,
                              "--project");
    std::error_code ec;
    std::filesystem::path root;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        root = path;
    } else if (std::filesystem::is_regular_file(path, ec) && !ec && path.filename() == "project.json") {
        root = path.parent_path();
    } else {
        throw std::runtime_error("--project must name a project directory or project.json: " +
                                 pathString(path));
    }
    if (!std::filesystem::is_regular_file(root / "project.json", ec) || ec) {
        throw std::runtime_error("project.json not found under --project: " + pathString(root));
    }
    return {root, root / "imports"};
}

bool pathStartsWith(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    const auto relative = candidate.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}

std::filesystem::path rulesPath(const ProjectContext &project, const std::filesystem::path &argument) {
    auto path = canonicalPath(argument.is_absolute() ? argument : project.root / argument, "--rules");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        throw std::runtime_error("--rules file not found: " + pathString(path));
    }
    if (!pathStartsWith(project.root, path)) {
        throw std::runtime_error("--rules must be inside the project so it can be tracked by git: " +
                                 pathString(path));
    }
    return path;
}

std::vector<SelectedInput> selectInputs(const ProjectContext &project,
                                        const std::filesystem::path &rules_path,
                                        const ImportRules &rules,
                                        const std::optional<std::filesystem::path> &source) {
    std::vector<std::filesystem::path> candidates;
    if (source) {
        auto path = canonicalPath(source->is_absolute() ? *source : project.root / *source, "--source");
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec || !pathStartsWith(project.root, path)) {
            throw std::runtime_error("--source must name a file inside the project: " + pathString(path));
        }
        candidates.push_back(path);
    } else {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it{
                 project.root, std::filesystem::directory_options::skip_permission_denied, ec},
             end;
             it != end; it.increment(ec)) {
            if (ec) {
                throw std::runtime_error("failed to scan project for imports: " + ec.message());
            }
            const auto &path = it->path();
            if (it->is_directory(ec)) {
                if (path == project.imports || path.filename() == ".pelican" || path.filename() == ".git") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (it->is_regular_file(ec) && path != rules_path) {
                candidates.push_back(canonicalPath(path, "import candidate"));
            }
        }
    }

    std::vector<SelectedInput> selected;
    for (const auto &path : candidates) {
        const auto relative = path.lexically_relative(project.root).generic_string();
        if (auto match = evaluateImportRules(rules, relative)) {
            selected.push_back({path, relative, std::move(*match)});
        }
    }
    std::sort(selected.begin(), selected.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.relative < rhs.relative;
    });
    return selected;
}

std::string withoutExtension(const std::string &relative) {
    auto path = std::filesystem::path{relative};
    path.replace_extension();
    return path.generic_string();
}

std::filesystem::path individualDelivery(const ProjectContext &project, const SelectedInput &input) {
    return project.imports / input.match.recipe.name / std::filesystem::path{withoutExtension(input.relative)};
}

std::string atlasGroupName(const ImportRuleMatch &match) {
    if (match.layer == ImportRuleLayer::rule) {
        std::ostringstream name;
        name << "rule_";
        name.width(4);
        name.fill('0');
        name << *match.rule_index;
        return name.str();
    }
    auto extension = match.default_extension;
    if (extension.starts_with('.')) {
        extension.erase(extension.begin());
    }
    return (match.layer == ImportRuleLayer::defaults ? "default_" : "builtin_") + extension;
}

std::string atlasGroupKey(const ImportRuleMatch &match) {
    return atlasGroupName(match) + "\n" + match.recipe.options.dump();
}

std::vector<std::filesystem::path> treeFiles(const std::filesystem::path &root) {
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec) || ec) {
        return result;
    }
    for (std::filesystem::recursive_directory_iterator it{root, ec}, end; it != end; it.increment(ec)) {
        if (ec) {
            throw std::runtime_error("failed to inspect delivery: " + pathString(root) + " (" + ec.message() + ")");
        }
        if (it->is_regular_file(ec)) {
            result.push_back(it->path().lexically_relative(root));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

bool treesEqual(const std::filesystem::path &lhs, const std::filesystem::path &rhs) {
    const auto left_files = treeFiles(lhs);
    const auto right_files = treeFiles(rhs);
    if (left_files != right_files) {
        return false;
    }
    return std::all_of(left_files.begin(), left_files.end(), [&](const auto &relative) {
        return readFile(lhs / relative, "generated output") == readFile(rhs / relative, "existing output");
    });
}

void publishDelivery(const std::filesystem::path &temporary, const std::filesystem::path &destination,
                     bool force) {
    std::error_code ec;
    if (std::filesystem::exists(destination, ec) && !ec) {
        if (treesEqual(temporary, destination)) {
            std::filesystem::remove_all(temporary, ec);
            return;
        }
        if (!force) {
            std::filesystem::remove_all(temporary, ec);
            throw std::runtime_error("generated delivery differs from existing output '" +
                                     pathString(destination) + "'; rerun with --force to overwrite");
        }
        std::filesystem::remove_all(destination, ec);
        if (ec) {
            throw std::runtime_error("failed to remove existing delivery: " + pathString(destination) +
                                     " (" + ec.message() + ")");
        }
    }
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("failed to create imports directory: " + ec.message());
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        throw std::runtime_error("failed to publish import delivery: " + pathString(destination) +
                                 " (" + ec.message() + ")");
    }
}

std::filesystem::path prepareTemporary(const std::filesystem::path &destination) {
    auto temporary = destination.parent_path() / ("." + destination.filename().string() + ".wp84-tmp");
    std::error_code ec;
    std::filesystem::remove_all(temporary, ec);
    ec.clear();
    std::filesystem::create_directories(temporary, ec);
    if (ec) {
        throw std::runtime_error("failed to create temporary delivery: " + pathString(temporary) +
                                 " (" + ec.message() + ")");
    }
    return temporary;
}

void validateGeneratedManifest(const std::filesystem::path &delivery) {
    const auto manifest_path = delivery / "manifest.json";
    auto document = nlohmann::json::parse(readFile(manifest_path, "generated manifest"));
    const auto manifest = parseImportManifestJson(document);
    for (const auto &output : manifest.outputs) {
        const auto path = delivery / std::filesystem::path{output.file};
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) {
            throw std::runtime_error("generated manifest output is missing: " + output.file);
        }
        if (sha256File(path) != output.sha256) {
            throw std::runtime_error("generated manifest sha256 mismatch: " + output.file);
        }
    }
}

void writeExtractDelivery(const SelectedInput &input, const std::filesystem::path &temporary) {
    const auto scene_path = temporary / "scene.json";
    writeFile(scene_path, extractGltfScene(input.absolute, input.relative).dump(2) + "\n");
    nlohmann::json manifest{
        {"schema", "pelican.import"},
        {"version", 1},
        {"tool", {{"name", "pelican_cli"}, {"version", "wp84"}}},
        {"source", {{"file", input.relative}}},
        {"outputs", nlohmann::json::array({{{"file", "scene.json"},
                                             {"schema", "pelican.scene"},
                                             {"version", 1},
                                             {"sha256", sha256File(scene_path)}}})},
    };
    writeFile(temporary / "manifest.json", manifest.dump(2) + "\n");
}

std::optional<std::filesystem::path> searchPath(std::string_view name) {
#ifdef _WIN32
    char *path_buffer = nullptr;
    std::size_t path_size = 0;
    if (_dupenv_s(&path_buffer, &path_size, "PATH") != 0 || path_buffer == nullptr) {
        return std::nullopt;
    }
    const std::string path_value{path_buffer};
    std::free(path_buffer);
    constexpr char separator = ';';
    const std::vector<std::string> suffixes{"", ".exe", ".cmd", ".bat"};
#else
    const char *path_buffer = std::getenv("PATH");
    if (!path_buffer) {
        return std::nullopt;
    }
    const std::string path_value{path_buffer};
    constexpr char separator = ':';
    const std::vector<std::string> suffixes{""};
#endif
    std::stringstream paths{path_value};
    std::string directory;
    while (std::getline(paths, directory, separator)) {
        for (const auto &suffix : suffixes) {
            auto candidate = std::filesystem::path{directory} / (std::string{name} + suffix);
            std::error_code ec;
            if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
                return canonicalPath(candidate, "external tool");
            }
        }
    }
    return std::nullopt;
}

ToolCommand resolveTool(const std::optional<std::filesystem::path> &configured) {
    if (configured) {
        std::error_code ec;
        if (std::filesystem::is_directory(*configured, ec) && !ec) {
            const auto uv = searchPath("uv");
            if (!uv) {
                throw std::runtime_error("pelican-import-tools project was configured but uv was not found on PATH; "
                                         "install uv, run 'uv sync' in " + pathString(*configured));
            }
            return {{pathString(*uv), "run", "--project", pathString(canonicalPath(*configured, "--import-tools")),
                     "pelican-import-tools"}};
        }
        if (std::filesystem::is_regular_file(*configured, ec) && !ec) {
            return {{{pathString(canonicalPath(*configured, "--import-tools"))}}};
        }
        throw std::runtime_error("pelican-import-tools configured path not found: " + pathString(*configured) +
                                 "; install with 'uv sync' in the pelican-import-tools repository");
    }
    if (const auto found = searchPath("pelican-import-tools")) {
        return {{{pathString(*found)}}};
    }
    throw std::runtime_error(
        "pelican-import-tools was not found on PATH; clone pelican-import-tools, run 'uv sync', then either "
        "install its pelican-import-tools command on PATH or pass --import-tools <repository-or-executable>");
}

std::string shellQuote(std::string_view value) {
#ifdef _WIN32
    std::string quoted{"\""};
    for (const char ch : value) {
        if (ch == '"') {
            quoted += "\\\"";
        } else {
            quoted += ch;
        }
    }
    return quoted + '"';
#else
    std::string quoted{"'"};
    for (const char ch : value) {
        quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
    }
    return quoted + '\'';
#endif
}

#ifdef _WIN32
std::string windowsArgument(std::string_view value) {
    if (value.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string{value};
    }
    std::string result{"\""};
    std::size_t backslashes = 0;
    for (const char ch : value) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result += '"';
        } else {
            result.append(backslashes, '\\');
            result += ch;
        }
        backslashes = 0;
    }
    result.append(backslashes * 2, '\\');
    result += '"';
    return result;
}
#endif

void runExternal(const ToolCommand &tool, const std::vector<std::string> &arguments) {
#ifdef _WIN32
    std::vector<std::string> command_arguments = tool.prefix;
    command_arguments.insert(command_arguments.end(), arguments.begin(), arguments.end());
    std::string command;
    for (const auto &argument : command_arguments) {
        if (!command.empty()) command += ' ';
        command += windowsArgument(argument);
    }
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(tool.prefix.front().c_str(), mutable_command.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &startup, &process)) {
        throw std::runtime_error("failed to start pelican-import-tools external process (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (result != 0) {
        throw std::runtime_error("pelican-import-tools external process failed with status " +
                                 std::to_string(result));
    }
#else
    std::string command;
    for (const auto &argument : tool.prefix) {
        if (!command.empty()) command += ' ';
        command += shellQuote(argument);
    }
    for (const auto &argument : arguments) {
        command += ' ';
        command += shellQuote(argument);
    }
    const int result = std::system(command.c_str());
    if (result != 0) {
        throw std::runtime_error("pelican-import-tools external process failed with status " +
                                 std::to_string(result));
    }
#endif
}

void rewriteExternalSource(const std::filesystem::path &temporary, nlohmann::json source) {
    const auto path = temporary / "manifest.json";
    auto manifest = nlohmann::json::parse(readFile(path, "pelican-import-tools manifest"));
    manifest["source"] = std::move(source);
    writeFile(path, manifest.dump(2) + "\n");
}

void processIndividual(const ProjectContext &project, const SelectedInput &input, bool force,
                       const std::optional<ToolCommand> &tool) {
    const auto destination = individualDelivery(project, input);
    const auto temporary = prepareTemporary(destination);
    try {
        if (input.match.recipe.name == "extract_scene") {
            writeExtractDelivery(input, temporary);
        } else if (input.match.recipe.name == "psd_layers") {
            runExternal(*tool, {"psd-extract", pathString(input.absolute), pathString(temporary)});
            rewriteExternalSource(temporary, {{"file", input.relative}});
        } else {
            throw std::runtime_error("internal error: non-atlas input selected unknown recipe");
        }
        validateGeneratedManifest(temporary);
        publishDelivery(temporary, destination, force);
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove_all(temporary, ec);
        throw;
    }
}

void processAtlas(const ProjectContext &project, const std::vector<const SelectedInput *> &inputs,
                  bool force, const ToolCommand &tool) {
    const auto group_name = atlasGroupName(inputs.front()->match);
    const auto destination = project.imports / "atlas_pack" / group_name;
    const auto temporary = prepareTemporary(destination);
    const auto staging = destination.parent_path() / ("." + group_name + ".wp84-input");
    std::error_code ec;
    std::filesystem::remove_all(staging, ec);
    try {
        for (const auto *input : inputs) {
            const auto staged = staging / std::filesystem::path{input->relative};
            std::filesystem::create_directories(staged.parent_path());
            std::filesystem::copy_file(input->absolute, staged, std::filesystem::copy_options::overwrite_existing);
        }
        std::vector<std::string> arguments{"atlas-pack", pathString(staging), pathString(temporary)};
        const auto &options = inputs.front()->match.recipe.options;
        if (const auto value = options.find("max_size"); value != options.end()) {
            arguments.insert(arguments.end(), {"--max-size", std::to_string(value->get<long long>())});
        }
        if (const auto value = options.find("padding"); value != options.end()) {
            arguments.insert(arguments.end(), {"--padding", std::to_string(value->get<long long>())});
        }
        runExternal(tool, arguments);
        auto files = nlohmann::json::array();
        for (const auto *input : inputs) files.push_back(input->relative);
        rewriteExternalSource(temporary, {{"files", std::move(files)}});
        validateGeneratedManifest(temporary);
        std::filesystem::remove_all(staging, ec);
        publishDelivery(temporary, destination, force);
    } catch (...) {
        std::filesystem::remove_all(staging, ec);
        std::filesystem::remove_all(temporary, ec);
        throw;
    }
}

} // namespace

int runRulesImportCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli import --rules");
    program.add_argument("--rules").required().metavar("imports.rules.json");
    program.add_argument("--project").required().metavar("dir|project.json");
    program.add_argument("--source").metavar("file");
    program.add_argument("--import-tools").metavar("repository|executable");
    program.add_argument("--force").default_value(false).implicit_value(true);
    try {
        program.parse_args(argc, argv);
        const auto project = loadProject(program.get<std::string>("--project"));
        const auto rules_path = rulesPath(project, program.get<std::string>("--rules"));
        const auto rules = parseImportRulesJson(nlohmann::json::parse(readFile(rules_path, "import rules")));
        std::optional<std::filesystem::path> source;
        if (program.is_used("--source")) source = program.get<std::string>("--source");
        const auto selected = selectInputs(project, rules_path, rules, source);
        if (selected.empty()) {
            return 0;
        }

        const bool needs_external = std::any_of(selected.begin(), selected.end(), [](const auto &input) {
            return input.match.recipe.name != "extract_scene";
        });
        std::optional<ToolCommand> tool;
        if (needs_external) {
            std::optional<std::filesystem::path> configured;
            if (program.is_used("--import-tools")) configured = program.get<std::string>("--import-tools");
            tool = resolveTool(configured);
        }

        const bool force = program.get<bool>("--force");
        std::map<std::string, std::vector<const SelectedInput *>> atlases;
        int deliveries = 0;
        for (const auto &input : selected) {
            if (input.match.recipe.name == "atlas_pack") {
                atlases[atlasGroupKey(input.match)].push_back(&input);
            } else {
                processIndividual(project, input, force, tool);
                ++deliveries;
            }
        }
        for (const auto &[key, inputs] : atlases) {
            (void)key;
            processAtlas(project, inputs, force, *tool);
            ++deliveries;
        }
        std::cout << "import rules matched " << selected.size() << " files and produced " << deliveries
                  << " deliveries\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n' << program;
        return -1;
    }
}

} // namespace Pelican::DevCli
