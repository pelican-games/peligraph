#include "importcommand.hpp"

#include "gltfsceneextract.hpp"
#include "rulesimport.hpp"

#include "../project/importmanifest.hpp"

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace Pelican::DevCli {

namespace {

constexpr std::string_view project_scheme = "project://";

struct ProjectPaths {
    std::filesystem::path root;
    std::filesystem::path project_file;
    std::filesystem::path asset_data_file;
};

std::string pathString(const std::filesystem::path &path) {
    return path.string();
}

std::filesystem::path absolutePath(const std::filesystem::path &path) {
    if (path.is_absolute()) {
        return path;
    }
    return std::filesystem::current_path() / path;
}

std::filesystem::path weaklyCanonicalOrThrow(const std::filesystem::path &path, std::string_view context) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        throw std::runtime_error(std::string{context} + " failed to normalize path: " + pathString(path) +
                                 " (" + ec.message() + ")");
    }
    return canonical;
}

std::filesystem::path canonicalFileOrThrow(const std::filesystem::path &path, std::string_view context) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        throw std::runtime_error(std::string{context} + " file not found: " + pathString(path));
    }
    return weaklyCanonicalOrThrow(path, context);
}

std::filesystem::path canonicalDirectoryOrThrow(const std::filesystem::path &path, std::string_view context) {
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) {
        throw std::runtime_error(std::string{context} + " directory not found: " + pathString(path));
    }
    return weaklyCanonicalOrThrow(path, context);
}

using PathString = std::filesystem::path::string_type;

PathString comparableComponent(const std::filesystem::path &component) {
    auto value = component.native();
#ifdef _WIN32
    std::transform(value.begin(), value.end(), value.begin(), [](std::filesystem::path::value_type ch) {
        if constexpr (std::is_same_v<std::filesystem::path::value_type, wchar_t>) {
            return static_cast<std::filesystem::path::value_type>(std::towlower(ch));
        } else {
            return static_cast<std::filesystem::path::value_type>(
                std::tolower(static_cast<unsigned char>(ch)));
        }
    });
#endif
    return value;
}

std::vector<PathString> pathComponents(const std::filesystem::path &path) {
    std::vector<PathString> components;
    for (const auto &component : path) {
        components.push_back(comparableComponent(component));
    }
    return components;
}

bool isWithinRoot(const std::filesystem::path &root, const std::filesystem::path &candidate) {
    const auto root_components = pathComponents(root);
    const auto candidate_components = pathComponents(candidate);
    if (candidate_components.size() < root_components.size()) {
        return false;
    }
    for (size_t i = 0; i < root_components.size(); ++i) {
        if (root_components[i] != candidate_components[i]) {
            return false;
        }
    }
    return true;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool startsWithSlashRoot(std::string_view value) {
    return value.starts_with("/") || value.starts_with("\\") || value.starts_with("//") ||
           value.starts_with("\\\\");
}

bool hasScheme(std::string_view value) {
    return value.find("://") != std::string_view::npos;
}

std::string stripProjectScheme(std::string_view ref) {
    if (startsWith(ref, project_scheme)) {
        return std::string{ref.substr(project_scheme.size())};
    }
    return std::string{ref};
}

std::string readTextFile(const std::filesystem::path &path, std::string_view context) {
    std::ifstream file{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error(std::string{"failed to open "} + std::string{context} + ": " +
                                 pathString(path));
    }
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

void writeTextFile(const std::filesystem::path &path, const std::string &contents) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file.is_open()) {
        throw std::runtime_error("failed to write file: " + pathString(path));
    }
    file << contents;
}

nlohmann::json readJsonFile(const std::filesystem::path &path, std::string_view context) {
    return nlohmann::json::parse(readTextFile(path, context));
}

std::string fileSha256(const std::filesystem::path &path) {
    const auto contents = readTextFile(path, "import output");
    return picosha2::hash256_hex_string(contents.begin(), contents.end());
}

std::filesystem::path resolveProjectFile(const std::filesystem::path &project_root, std::string_view ref,
                                         std::string_view context) {
    if (ref.empty()) {
        throw std::runtime_error(std::string{context} + " must not be empty");
    }
    if (ref.find('\\') != std::string_view::npos) {
        throw std::runtime_error(std::string{context} + " must use forward slashes: " + std::string{ref});
    }
    if (hasScheme(ref) && !startsWith(ref, project_scheme)) {
        throw std::runtime_error(std::string{context} + " uses unsupported scheme: " + std::string{ref});
    }

    const auto stripped = stripProjectScheme(ref);
    if (startsWithSlashRoot(stripped)) {
        throw std::runtime_error(std::string{context} + " must be project-relative: " + std::string{ref});
    }

    const std::filesystem::path rel{stripped};
    if (rel.is_absolute() || rel.has_root_name()) {
        throw std::runtime_error(std::string{context} + " must be project-relative: " + std::string{ref});
    }

    const auto canonical = weaklyCanonicalOrThrow(project_root / rel, context);
    if (!isWithinRoot(project_root, canonical)) {
        throw std::runtime_error(std::string{context} + " escapes project root: " + std::string{ref});
    }
    return canonicalFileOrThrow(canonical, context);
}

ProjectPaths loadProjectPaths(const std::filesystem::path &project_arg) {
    auto path = weaklyCanonicalOrThrow(absolutePath(project_arg), "--project");
    std::filesystem::path project_file;
    std::filesystem::path project_root;

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        project_root = canonicalDirectoryOrThrow(path, "--project");
        project_file = canonicalFileOrThrow(project_root / "project.json", "project.json");
    } else if (std::filesystem::is_regular_file(path, ec) && !ec) {
        if (path.filename() != "project.json") {
            throw std::runtime_error("--project file must be named project.json: " + pathString(path));
        }
        project_file = canonicalFileOrThrow(path, "--project");
        project_root = canonicalDirectoryOrThrow(project_file.parent_path(), "--project");
    } else {
        throw std::runtime_error("--project must point to a directory or project.json file: " +
                                 pathString(path));
    }

    const auto project = readJsonFile(project_file, "project.json");
    const auto basic = project.find("basic_config");
    if (basic == project.end() || !basic->is_object()) {
        throw std::runtime_error("project.json requires basic_config object");
    }
    const auto asset_ref = basic->find("asset_data_json");
    if (asset_ref == basic->end() || !asset_ref->is_string()) {
        throw std::runtime_error("project.json requires string basic_config.asset_data_json");
    }

    return ProjectPaths{
        project_root,
        project_file,
        resolveProjectFile(project_root, asset_ref->get<std::string>(),
                           "project.json basic_config.asset_data_json"),
    };
}

std::filesystem::path outputPathInDelivery(const std::filesystem::path &delivery_root,
                                           const ImportManifestOutput &output) {
    const auto joined = delivery_root / std::filesystem::path{output.file};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(joined, ec) || ec) {
        throw std::runtime_error("import output file not found: " + output.file);
    }
    const auto canonical = weaklyCanonicalOrThrow(joined, "import output");
    if (!isWithinRoot(delivery_root, canonical)) {
        throw std::runtime_error("import output escapes delivery directory: " + output.file);
    }
    return canonical;
}

std::string lowerExtension(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

bool isRegisterableGlb(const ImportManifestOutput &output) {
    return output.schema == "gltf" && lowerExtension(std::filesystem::path{output.file}) == ".glb";
}

std::string projectRelativeString(const std::filesystem::path &project_root,
                                  const std::filesystem::path &path) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(path, project_root, ec);
    if (ec) {
        throw std::runtime_error("failed to make project-relative path for imported asset: " +
                                 pathString(path) + " (" + ec.message() + ")");
    }
    return relative.generic_string();
}

std::string modelNameForOutput(const ImportManifestOutput &output) {
    return std::filesystem::path{output.file}.stem().string();
}

void verifyManifestOutputs(const std::filesystem::path &delivery_root, const ImportManifest &manifest,
                           std::vector<std::filesystem::path> &resolved_outputs) {
    resolved_outputs.clear();
    resolved_outputs.reserve(manifest.outputs.size());
    for (const auto &output : manifest.outputs) {
        const auto output_path = outputPathInDelivery(delivery_root, output);
        const auto actual_sha = fileSha256(output_path);
        if (actual_sha != output.sha256) {
            throw std::runtime_error("sha256 mismatch for import output " + output.file +
                                     ": expected " + output.sha256 + ", got " + actual_sha);
        }
        resolved_outputs.push_back(output_path);
    }
}

ImportCommandResult registerImportedModels(const ProjectPaths &project, const ImportManifest &manifest,
                                           const std::vector<std::filesystem::path> &resolved_outputs) {
    auto asset_data = readJsonFile(project.asset_data_file, "asset_data_json");
    if (!asset_data.is_object()) {
        throw std::runtime_error("asset_data_json must be an object");
    }
    if (!asset_data.contains("models")) {
        asset_data["models"] = nlohmann::json::array();
    }
    if (!asset_data.at("models").is_array()) {
        throw std::runtime_error("asset_data_json models must be an array");
    }

    std::unordered_set<std::string> existing_names;
    std::unordered_set<std::string> existing_paths;
    for (const auto &model : asset_data.at("models")) {
        if (!model.is_object()) {
            throw std::runtime_error("asset_data_json models entries must be objects");
        }
        if (const auto name = model.find("name"); name != model.end() && name->is_string()) {
            existing_names.insert(name->get<std::string>());
        }
        if (const auto path = model.find("path"); path != model.end() && path->is_string()) {
            existing_paths.insert(path->get<std::string>());
        }
    }

    ImportCommandResult result;
    result.verified_outputs = static_cast<int>(manifest.outputs.size());
    for (size_t i = 0; i < manifest.outputs.size(); ++i) {
        const auto &output = manifest.outputs.at(i);
        if (!isRegisterableGlb(output)) {
            continue;
        }

        const auto model_name = modelNameForOutput(output);
        const auto model_path = projectRelativeString(project.root, resolved_outputs.at(i));
        if (existing_names.contains(model_name) || existing_paths.contains(model_path)) {
            ++result.skipped_models;
            continue;
        }

        asset_data["models"].push_back(nlohmann::json{
            {"name", model_name},
            {"path", model_path},
        });
        existing_names.insert(model_name);
        existing_paths.insert(model_path);
        ++result.registered_models;
    }

    writeTextFile(project.asset_data_file, asset_data.dump(2) + "\n");
    return result;
}

} // namespace

ImportCommandResult importDelivery(const std::filesystem::path &delivery_dir,
                                   const std::filesystem::path &project_arg) {
    const auto project = loadProjectPaths(project_arg);
    const auto delivery_root = canonicalDirectoryOrThrow(absolutePath(delivery_dir), "delivery directory");
    if (!isWithinRoot(project.root, delivery_root)) {
        throw std::runtime_error("delivery directory must be inside the project root: " +
                                 pathString(delivery_root));
    }

    const auto manifest_path = canonicalFileOrThrow(delivery_root / "manifest.json", "import manifest");
    const auto manifest = parseImportManifestJson(readJsonFile(manifest_path, "import manifest"));

    std::vector<std::filesystem::path> resolved_outputs;
    verifyManifestOutputs(delivery_root, manifest, resolved_outputs);
    return registerImportedModels(project, manifest, resolved_outputs);
}

int runImportCommand(int argc, char *argv[]) {
    if (argc > 1 && std::string_view{argv[1]} == "--rules") {
        return runRulesImportCommand(argc, argv);
    }
    if (argc > 1 && std::string_view{argv[1]} == "gltf") {
        return runGltfImportCommand(argc - 1, argv + 1);
    }
    argparse::ArgumentParser program("Pelican Cli import");
    program.add_argument("delivery_dir").help("delivery directory containing manifest.json");
    program.add_argument("--project").required().metavar("dir|project.json").help("project directory or project.json");

    try {
        program.parse_args(argc, argv);
        const auto result =
            importDelivery(program.get<std::string>("delivery_dir"), program.get<std::string>("--project"));
        std::cout << "verified " << result.verified_outputs << " outputs, registered "
                  << result.registered_models << " models, skipped " << result.skipped_models << " models"
                  << std::endl;
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return -1;
    }
    return 0;
}

} // namespace Pelican::DevCli
