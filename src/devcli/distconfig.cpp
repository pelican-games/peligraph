#include "distconfig.hpp"

#include "../project/assetdataformat.hpp"
#include "../project/importmanifest.hpp"
#include "../project/projectformat.hpp"
#include "../core/model/vatformat.hpp"

#include <argparse/argparse.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace Pelican::DevCli {

namespace {

constexpr std::string_view project_scheme = "project://";
constexpr std::string_view import_schema = "pelican.import";
constexpr uint32_t glb_magic = 0x46546c67;
constexpr uint32_t glb_version = 2;
constexpr uint32_t glb_json_chunk = 0x4e4f534a;
constexpr int gltf_mode_triangles = 4;

struct ProjectFiles {
    std::filesystem::path root;
    std::filesystem::path project_file;
    std::filesystem::path asset_data_file;
    std::optional<std::filesystem::path> ui_config_file;
};

struct GlbCandidate {
    std::filesystem::path path;
    std::string source;
};

struct ReferenceScan {
    std::vector<GlbCandidate> glb_candidates;
    std::vector<std::string> missing_glb_refs;
    std::vector<std::string> exr_refs;
    std::unordered_set<std::string> seen_glb_paths;
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

std::vector<uint8_t> readBinaryFile(const std::filesystem::path &path, std::string_view context) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file.is_open()) {
        throw std::runtime_error(std::string{"failed to open "} + std::string{context} + ": " +
                                 pathString(path));
    }
    const auto size = file.tellg();
    if (size < 0) {
        throw std::runtime_error(std::string{"failed to size "} + std::string{context} + ": " +
                                 pathString(path));
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.seekg(0);
    if (!bytes.empty()) {
        file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!file.good() && !file.eof()) {
        throw std::runtime_error(std::string{"failed to read "} + std::string{context} + ": " +
                                 pathString(path));
    }
    return bytes;
}

void writeTextFile(const std::filesystem::path &path, const std::string &contents) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file.is_open()) {
        throw std::runtime_error("failed to write file: " + pathString(path));
    }
    file << contents;
}

nlohmann::json readJsonFile(const std::filesystem::path &path, std::string_view context) {
    return nlohmann::json::parse(readTextFile(path, context));
}

std::filesystem::path resolveProjectRef(const std::filesystem::path &project_root, std::string_view ref,
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
    return canonical;
}

std::filesystem::path resolveProjectFile(const std::filesystem::path &project_root, std::string_view ref,
                                         std::string_view context) {
    return canonicalFileOrThrow(resolveProjectRef(project_root, ref, context), context);
}

const nlohmann::json &requireObjectMember(const nlohmann::json &object, std::string_view key,
                                          std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end()) {
        throw std::runtime_error(std::string{context} + " requires " + std::string{key});
    }
    return found.value();
}

std::string requireStringMember(const nlohmann::json &object, std::string_view key,
                                std::string_view context) {
    const auto &value = requireObjectMember(object, key, context);
    if (!value.is_string()) {
        throw std::runtime_error(std::string{context} + " requires string " + std::string{key});
    }
    return value.get<std::string>();
}

std::optional<std::string> optionalStringMember(const nlohmann::json &object, std::string_view key,
                                                std::string_view context) {
    const auto found = object.find(key);
    if (found == object.end()) {
        return std::nullopt;
    }
    if (!found->is_string()) {
        throw std::runtime_error(std::string{context} + " optional " + std::string{key} +
                                 " must be a string");
    }
    return found->get<std::string>();
}

ProjectFiles loadProjectFiles(const std::filesystem::path &project_arg) {
    auto path = weaklyCanonicalOrThrow(absolutePath(project_arg), "project");
    std::filesystem::path project_file;
    std::filesystem::path project_root;

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec) && !ec) {
        project_root = canonicalDirectoryOrThrow(path, "project");
        project_file = canonicalFileOrThrow(project_root / "project.json", "project.json");
    } else if (std::filesystem::is_regular_file(path, ec) && !ec) {
        if (path.filename() != "project.json") {
            throw std::runtime_error("project file must be named project.json: " + pathString(path));
        }
        project_file = canonicalFileOrThrow(path, "project");
        project_root = canonicalDirectoryOrThrow(project_file.parent_path(), "project");
    } else {
        throw std::runtime_error("project must point to a directory or project.json file: " +
                                 pathString(path));
    }

    const auto project = readJsonFile(project_file, "project.json");
    const auto envelope = parseProjectEnvelopeJson(project);
    const auto &basic = envelope.envelope.basic_config;
    if (!basic.is_object()) {
        throw std::runtime_error("project.json requires basic_config object");
    }

    const auto asset_ref = requireStringMember(basic, "asset_data_json", "project.json basic_config");
    std::optional<std::filesystem::path> ui_config_file;
    if (const auto ui_ref = optionalStringMember(basic, "ui_config_json", "project.json basic_config")) {
        ui_config_file =
            resolveProjectFile(project_root, *ui_ref, "project.json basic_config.ui_config_json");
    }

    return ProjectFiles{
        project_root,
        project_file,
        resolveProjectFile(project_root, asset_ref, "project.json basic_config.asset_data_json"),
        ui_config_file,
    };
}

std::string lowerExtension(const std::filesystem::path &path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext;
}

bool refHasExtension(std::string_view ref, std::string_view extension) {
    return lowerExtension(std::filesystem::path{stripProjectScheme(ref)}) == extension;
}

bool isReferenceKey(std::string_view key) {
    return key == "path" || key == "file";
}

void collectExrReferences(const nlohmann::json &json, std::string context, std::vector<std::string> &refs) {
    if (json.is_object()) {
        for (const auto &[key, value] : json.items()) {
            const auto child_context = context + "." + key;
            if (isReferenceKey(key) && value.is_string() && refHasExtension(value.get<std::string>(), ".exr")) {
                refs.push_back(child_context + " -> " + value.get<std::string>());
            }
            collectExrReferences(value, child_context, refs);
        }
        return;
    }

    if (json.is_array()) {
        for (size_t i = 0; i < json.size(); ++i) {
            collectExrReferences(json.at(i), context + "[" + std::to_string(i) + "]", refs);
        }
    }
}

void addGlbCandidate(ReferenceScan &scan, const std::filesystem::path &path, std::string source) {
    const auto key = pathString(path);
    if (scan.seen_glb_paths.insert(key).second) {
        scan.glb_candidates.push_back(GlbCandidate{path, std::move(source)});
    }
}

void addMaybeExistingGlbCandidate(ReferenceScan &scan, const std::filesystem::path &project_root,
                                  std::string_view ref, std::string source, bool already_resolved = false) {
    const auto path = already_resolved ? std::filesystem::path{std::string{ref}}
                                       : resolveProjectRef(project_root, ref, source);
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
        const auto canonical = weaklyCanonicalOrThrow(path, source);
        if (!isWithinRoot(project_root, canonical)) {
            throw std::runtime_error(source + " escapes project root: " + pathString(canonical));
        }
        addGlbCandidate(scan, canonical, std::move(source));
    } else {
        scan.missing_glb_refs.push_back(source + " -> " + pathString(path));
    }
}

void collectAssetDataGlbs(const nlohmann::json &asset_data, const std::filesystem::path &project_root,
                          ReferenceScan &scan) {
    (void)parseAssetDataFormatJson(asset_data);

    const auto models = asset_data.find("models");
    if (models == asset_data.end()) {
        return;
    }
    if (!models->is_array()) {
        throw std::runtime_error("asset_data_json models must be an array");
    }

    for (size_t i = 0; i < models->size(); ++i) {
        const auto &model = models->at(i);
        if (!model.is_object()) {
            throw std::runtime_error("asset_data_json models entries must be objects");
        }
        const auto path = model.find("path");
        if (path == model.end()) {
            continue;
        }
        if (!path->is_string()) {
            throw std::runtime_error("asset_data_json models[].path must be a string");
        }

        const auto ref = path->get<std::string>();
        if (refHasExtension(ref, ".glb")) {
            addMaybeExistingGlbCandidate(scan, project_root, ref,
                                         "asset_data_json models[" + std::to_string(i) + "].path");
        }
    }
}

void collectImportManifestGlbs(const std::filesystem::path &project_root, ReferenceScan &scan) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it{
        project_root, std::filesystem::directory_options::skip_permission_denied, ec};
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec || it->path().filename() != "manifest.json") {
            continue;
        }

        const auto manifest_path = it->path();
        const auto manifest_json = readJsonFile(manifest_path, "import manifest");
        if (!manifest_json.is_object() || manifest_json.value("schema", std::string{}) != import_schema) {
            continue;
        }

        const auto manifest = parseImportManifestJson(manifest_json);
        for (size_t i = 0; i < manifest.outputs.size(); ++i) {
            const auto &output = manifest.outputs.at(i);
            if (output.schema != "gltf" || !refHasExtension(output.file, ".glb")) {
                continue;
            }

            const auto output_path =
                weaklyCanonicalOrThrow(manifest_path.parent_path() / output.file, "import manifest output");
            addMaybeExistingGlbCandidate(scan, project_root, pathString(output_path),
                                         "import manifest " + pathString(manifest_path) +
                                             " outputs[" + std::to_string(i) + "].file",
                                         true);
        }
    }
}

uint32_t readLe32(const std::vector<uint8_t> &bytes, size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw std::runtime_error("GLB ended before uint32 field");
    }
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

nlohmann::json readGlbJsonChunk(const std::filesystem::path &path) {
    const auto bytes = readBinaryFile(path, "GLB");
    if (bytes.size() < 20) {
        throw std::runtime_error("GLB is too small: " + pathString(path));
    }
    if (readLe32(bytes, 0) != glb_magic) {
        throw std::runtime_error("GLB magic is invalid: " + pathString(path));
    }
    if (readLe32(bytes, 4) != glb_version) {
        throw std::runtime_error("GLB version is not supported: " + pathString(path));
    }
    const auto declared_length = readLe32(bytes, 8);
    if (declared_length > bytes.size()) {
        throw std::runtime_error("GLB declared length exceeds file size: " + pathString(path));
    }

    const auto json_length = readLe32(bytes, 12);
    if (readLe32(bytes, 16) != glb_json_chunk) {
        throw std::runtime_error("GLB first chunk is not JSON: " + pathString(path));
    }
    if (20ull + json_length > bytes.size()) {
        throw std::runtime_error("GLB JSON chunk exceeds file size: " + pathString(path));
    }

    return nlohmann::json::parse(
        std::string{reinterpret_cast<const char *>(bytes.data() + 20), json_length});
}

const nlohmann::json *objectMember(const nlohmann::json &object, const char *name) {
    if (!object.is_object()) {
        return nullptr;
    }
    const auto found = object.find(name);
    return found == object.end() ? nullptr : &found.value();
}

std::optional<std::string> valueString(const nlohmann::json *value) {
    if (value == nullptr || !value->is_string()) {
        return std::nullopt;
    }
    return value->get<std::string>();
}

std::optional<int64_t> valueInteger(const nlohmann::json *value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    if (value->is_number_unsigned()) {
        const auto parsed = value->get<uint64_t>();
        if (parsed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<int64_t>(parsed);
    }
    if (value->is_number_integer()) {
        return value->get<int64_t>();
    }
    return std::nullopt;
}

std::optional<double> valueNumber(const nlohmann::json *value) {
    if (value == nullptr || !value->is_number()) {
        return std::nullopt;
    }
    return value->get<double>();
}

std::optional<bool> valueBool(const nlohmann::json *value) {
    if (value == nullptr || !value->is_boolean()) {
        return std::nullopt;
    }
    return value->get<bool>();
}

std::optional<std::array<double, 3>> valueVec3(const nlohmann::json *value) {
    if (value == nullptr || !value->is_array() || value->size() != 3) {
        return std::nullopt;
    }

    std::array<double, 3> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        const auto component = valueNumber(&value->at(i));
        if (!component) {
            return std::nullopt;
        }
        result[i] = *component;
    }
    return result;
}

VatPrimitiveMeta jsonToVatMeta(const nlohmann::json &extras) {
    VatPrimitiveMeta meta;
    if (!extras.is_object()) {
        return meta;
    }

    const auto *vat_value = objectMember(extras, "pelican.vat");
    if (vat_value == nullptr) {
        return meta;
    }

    meta.present = true;
    if (!vat_value->is_object()) {
        meta.single_clip_object = false;
        return meta;
    }

    meta.schema = valueString(objectMember(*vat_value, "schema"));
    meta.version = valueInteger(objectMember(*vat_value, "version"));
    meta.generator = valueString(objectMember(*vat_value, "generator"));
    meta.fps = valueNumber(objectMember(*vat_value, "fps"));
    meta.frame_count = valueInteger(objectMember(*vat_value, "frame_count"));
    meta.vertex_count = valueInteger(objectMember(*vat_value, "vertex_count"));
    meta.bounds_min = valueVec3(objectMember(*vat_value, "bounds_min"));
    meta.bounds_max = valueVec3(objectMember(*vat_value, "bounds_max"));
    meta.loop = valueBool(objectMember(*vat_value, "loop"));
    meta.position_view = valueInteger(objectMember(*vat_value, "position_view"));
    meta.normal_view = valueInteger(objectMember(*vat_value, "normal_view"));
    return meta;
}

uint64_t requiredNonNegativeInteger(const nlohmann::json &value, std::string_view context) {
    if (value.is_number_unsigned()) {
        return value.get<uint64_t>();
    }
    if (value.is_number_integer()) {
        const auto parsed = value.get<int64_t>();
        if (parsed >= 0) {
            return static_cast<uint64_t>(parsed);
        }
    }
    throw std::runtime_error(std::string{context} + " must be a non-negative integer");
}

uint32_t requiredUint32(const nlohmann::json &value, std::string_view context) {
    const auto parsed = requiredNonNegativeInteger(value, context);
    if (parsed > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string{context} + " exceeds uint32 range");
    }
    return static_cast<uint32_t>(parsed);
}

size_t requiredSize(const nlohmann::json &value, std::string_view context) {
    const auto parsed = requiredNonNegativeInteger(value, context);
    if (parsed > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error(std::string{context} + " exceeds size_t range");
    }
    return static_cast<size_t>(parsed);
}

int requiredInt(const nlohmann::json &value, std::string_view context) {
    const auto parsed = requiredNonNegativeInteger(value, context);
    if (parsed > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string{context} + " exceeds int range");
    }
    return static_cast<int>(parsed);
}

std::vector<VatBufferViewInfo> parseVatBufferViews(const nlohmann::json &gltf) {
    const auto views = gltf.find("bufferViews");
    if (views == gltf.end()) {
        return {};
    }
    if (!views->is_array()) {
        throw std::runtime_error("GLB bufferViews must be an array");
    }

    std::vector<VatBufferViewInfo> result;
    result.reserve(views->size());
    for (size_t i = 0; i < views->size(); ++i) {
        const auto &view = views->at(i);
        if (!view.is_object()) {
            throw std::runtime_error("GLB bufferViews entries must be objects");
        }

        const auto buffer = objectMember(view, "buffer");
        const auto byte_length = objectMember(view, "byteLength");
        if (buffer == nullptr || byte_length == nullptr) {
            throw std::runtime_error("GLB bufferViews entries require buffer and byteLength");
        }

        size_t byte_offset = 0;
        if (const auto offset = objectMember(view, "byteOffset")) {
            byte_offset = requiredSize(*offset, "GLB bufferView.byteOffset");
        }

        result.push_back(VatBufferViewInfo{
            .index = static_cast<int>(i),
            .buffer = requiredInt(*buffer, "GLB bufferView.buffer"),
            .byte_offset = byte_offset,
            .byte_length = requiredSize(*byte_length, "GLB bufferView.byteLength"),
        });
    }
    return result;
}

uint32_t primitivePositionVertexCount(const nlohmann::json &gltf, const nlohmann::json &primitive) {
    const auto attributes = objectMember(primitive, "attributes");
    if (attributes == nullptr || !attributes->is_object()) {
        throw std::runtime_error("pelican.vat primitive requires attributes object");
    }
    const auto position = objectMember(*attributes, "POSITION");
    if (position == nullptr) {
        throw std::runtime_error("pelican.vat primitive requires POSITION accessor");
    }
    const auto accessor_index = requiredInt(*position, "pelican.vat POSITION accessor");

    const auto accessors = objectMember(gltf, "accessors");
    if (accessors == nullptr || !accessors->is_array()) {
        throw std::runtime_error("pelican.vat GLB requires accessors array");
    }
    if (static_cast<size_t>(accessor_index) >= accessors->size()) {
        throw std::runtime_error("pelican.vat POSITION accessor is out of range");
    }

    const auto count = objectMember(accessors->at(static_cast<size_t>(accessor_index)), "count");
    if (count == nullptr) {
        throw std::runtime_error("pelican.vat POSITION accessor requires count");
    }
    return requiredUint32(*count, "pelican.vat POSITION accessor count");
}

bool glbContainsVat(const std::filesystem::path &path, std::string &detail) {
    const auto gltf = readGlbJsonChunk(path);
    const auto meshes = objectMember(gltf, "meshes");
    if (meshes == nullptr) {
        return false;
    }
    if (!meshes->is_array()) {
        throw std::runtime_error("GLB meshes must be an array: " + pathString(path));
    }

    std::optional<std::vector<VatBufferViewInfo>> buffer_views;
    for (size_t mesh_index = 0; mesh_index < meshes->size(); ++mesh_index) {
        const auto &mesh = meshes->at(mesh_index);
        if (!mesh.is_object()) {
            throw std::runtime_error("GLB meshes entries must be objects: " + pathString(path));
        }
        const auto primitives = objectMember(mesh, "primitives");
        if (primitives == nullptr) {
            continue;
        }
        if (!primitives->is_array()) {
            throw std::runtime_error("GLB mesh primitives must be an array: " + pathString(path));
        }

        for (size_t primitive_index = 0; primitive_index < primitives->size(); ++primitive_index) {
            const auto &primitive = primitives->at(primitive_index);
            if (!primitive.is_object()) {
                throw std::runtime_error("GLB primitive entries must be objects: " + pathString(path));
            }

            const auto extras = objectMember(primitive, "extras");
            const auto meta = extras == nullptr ? VatPrimitiveMeta{} : jsonToVatMeta(*extras);
            if (!meta.present) {
                continue;
            }

            if (!buffer_views) {
                buffer_views = parseVatBufferViews(gltf);
            }
            const auto vat_info = parseVatPrimitiveExtras(
                meta, primitivePositionVertexCount(gltf, primitive), *buffer_views);
            if (!vat_info) {
                continue;
            }

            const auto mode = objectMember(primitive, "mode");
            if (mode != nullptr && requiredInt(*mode, "pelican.vat primitive mode") != gltf_mode_triangles) {
                throw std::runtime_error("pelican.vat only supports TRIANGLES topology");
            }

            detail = "mesh[" + std::to_string(mesh_index) + "].primitives[" +
                     std::to_string(primitive_index) + "]";
            return true;
        }
    }

    return false;
}

std::string firstWithCount(const std::vector<std::string> &values) {
    if (values.empty()) {
        return {};
    }
    if (values.size() == 1) {
        return values.front();
    }
    return values.front() + " (+" + std::to_string(values.size() - 1) + " more)";
}

std::string vatOffReason(const ReferenceScan &scan) {
    std::ostringstream reason;
    reason << "scanned " << scan.glb_candidates.size() << " existing GLB candidate";
    if (scan.glb_candidates.size() != 1) {
        reason << "s";
    }
    reason << "; no pelican.vat extras found";
    if (!scan.missing_glb_refs.empty()) {
        reason << "; skipped missing " << firstWithCount(scan.missing_glb_refs);
    }
    return reason.str();
}

bool deriveVatEnabled(const ReferenceScan &scan, std::string &reason) {
    for (const auto &candidate : scan.glb_candidates) {
        std::string detail;
        if (glbContainsVat(candidate.path, detail)) {
            reason = candidate.source + " -> " + pathString(candidate.path) +
                     " contains pelican.vat extras at " + detail;
            return true;
        }
    }

    reason = vatOffReason(scan);
    return false;
}

std::string boolString(bool value) {
    return value ? "ON" : "OFF";
}

std::string oneLine(std::string value) {
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

std::string utcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now_time);
#else
    gmtime_r(&now_time, &tm);
#endif

    std::ostringstream stream;
    stream << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

DistConfigOptions parseWithList(std::string_view value) {
    DistConfigOptions options;
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = trim(value.substr(0, comma));
        if (token.empty()) {
            throw std::runtime_error("--with contains an empty feature name");
        }
        if (token == "rpc") {
            options.with_rpc = true;
        } else if (token == "seqplayer") {
            options.with_seqplayer = true;
        } else if (token == "openxr") {
            options.with_openxr = true;
        } else {
            throw std::runtime_error("unsupported --with feature: " + std::string{token});
        }

        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    }
    return options;
}

} // namespace

DistConfigResult deriveDistConfig(const std::filesystem::path &project_arg,
                                  const DistConfigOptions &options) {
    const auto project = loadProjectFiles(project_arg);
    const auto asset_data = readJsonFile(project.asset_data_file, "asset_data_json");

    ReferenceScan scan;
    collectExrReferences(asset_data, "asset_data_json", scan.exr_refs);
    collectAssetDataGlbs(asset_data, project.root, scan);
    if (project.ui_config_file) {
        collectExrReferences(readJsonFile(*project.ui_config_file, "ui_config_json"), "ui_config_json",
                             scan.exr_refs);
    }
    collectImportManifestGlbs(project.root, scan);

    DistConfigResult result;
    result.project_root = project.root;
    result.project_file = project.project_file;
    result.with_rpc = options.with_rpc;
    result.with_seqplayer = options.with_seqplayer;
    result.with_openxr = options.with_openxr;
    result.with_exr = !scan.exr_refs.empty();
    result.exr_reason = result.with_exr
                            ? firstWithCount(scan.exr_refs)
                            : "no .exr references in asset_data_json or ui_config_json";
    result.with_vat = deriveVatEnabled(scan, result.vat_reason);
    result.rpc_reason = result.with_rpc ? "enabled by --with rpc"
                                        : "distribution default; pass --with rpc to enable";
    result.seqplayer_reason = result.with_seqplayer ? "enabled by --with seqplayer"
                                                    : "distribution default; pass --with seqplayer to enable";
    result.openxr_reason = result.with_openxr ? "enabled by --with openxr"
                                              : "distribution default; pass --with openxr to enable";
    return result;
}

std::string renderDistConfigPreset(const DistConfigResult &result) {
    std::ostringstream out;
    out << "# Generated by pelican_cli dist-config\n";
    out << "# Project: " << result.project_file.generic_string() << "\n";
    out << "# Generated at: " << utcTimestamp() << "\n";
    out << "# PELICAN_WITH_VAT: " << boolString(result.with_vat) << " - "
        << oneLine(result.vat_reason) << "\n";
    out << "# PELICAN_WITH_EXR: " << boolString(result.with_exr) << " - "
        << oneLine(result.exr_reason) << "\n";
    out << "# PELICAN_WITH_RPC: " << boolString(result.with_rpc) << " - "
        << oneLine(result.rpc_reason) << "\n";
    out << "# PELICAN_WITH_SEQPLAYER: " << boolString(result.with_seqplayer) << " - "
        << oneLine(result.seqplayer_reason) << "\n";
    out << "# PELICAN_WITH_OPENXR: " << boolString(result.with_openxr) << " - "
        << oneLine(result.openxr_reason) << "\n\n";
    out << "# PELICAN_WITH_IMGUI: OFF - distribution builds exclude engine developer UI\n";
    out << "# PELICAN_WITH_RENDERDOC: OFF - distribution builds exclude capture integration\n";
    out << "# PELICAN_WITH_SPIRV_LINK: OFF - experimental linker is a developer-only build unit\n";
    out << "# BUILD_TESTING: OFF - distribution builds exclude developer tests and test tooling\n\n";

    out << "set(PELICAN_WITH_VAT " << boolString(result.with_vat) << " CACHE BOOL \"\" FORCE)\n";
    out << "set(PELICAN_WITH_EXR " << boolString(result.with_exr) << " CACHE BOOL \"\" FORCE)\n";
    out << "set(PELICAN_WITH_RPC " << boolString(result.with_rpc) << " CACHE BOOL \"\" FORCE)\n";
    out << "set(PELICAN_WITH_SEQPLAYER " << boolString(result.with_seqplayer)
        << " CACHE BOOL \"\" FORCE)\n";
    out << "set(PELICAN_WITH_OPENXR " << boolString(result.with_openxr) << " CACHE BOOL \"\" FORCE)\n";
    out << "set(PELICAN_WITH_IMGUI OFF CACHE BOOL \"\" FORCE)\n";
    out << "set(PELICAN_WITH_RENDERDOC OFF CACHE BOOL \"\" FORCE)\n";
    out << "set(PELICAN_WITH_SPIRV_LINK OFF CACHE BOOL \"\" FORCE)\n";
    out << "set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)\n";
    return out.str();
}

int runDistConfigCommand(int argc, char *argv[]) {
    argparse::ArgumentParser program("Pelican Cli dist-config");
    program.add_argument("project").help("project directory or project.json");
    program.add_argument("--with")
        .default_value(std::string{})
        .metavar("rpc,seqplayer,openxr")
        .help("comma-separated distribution features to force on");
    program.add_argument("--out")
        .default_value(std::string{"dist-preset.cmake"})
        .metavar("file")
        .help("output CMake cache preset file");

    try {
        program.parse_args(argc, argv);
        const auto options = parseWithList(program.get<std::string>("--with"));
        const auto result = deriveDistConfig(program.get<std::string>("project"), options);
        const auto out_path = absolutePath(program.get<std::string>("--out"));
        writeTextFile(out_path, renderDistConfigPreset(result));
        std::cout << "wrote " << pathString(out_path) << std::endl;
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return -1;
    }
    return 0;
}

} // namespace Pelican::DevCli
