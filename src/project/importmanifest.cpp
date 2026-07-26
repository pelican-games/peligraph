#include "importmanifest.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Pelican {

namespace {

constexpr std::string_view import_schema = "pelican.import";
constexpr int supported_import_version = 1;

bool startsWithSlashRoot(std::string_view value) {
    return value.starts_with("/") || value.starts_with("\\") || value.starts_with("//") ||
           value.starts_with("\\\\");
}

bool containsBackslash(std::string_view value) {
    return value.find('\\') != std::string_view::npos;
}

bool hasScheme(std::string_view value) {
    return value.find("://") != std::string_view::npos;
}

bool isHexSha256(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    for (const char ch : value) {
        const bool is_digit = ch >= '0' && ch <= '9';
        const bool is_lower = ch >= 'a' && ch <= 'f';
        const bool is_upper = ch >= 'A' && ch <= 'F';
        if (!is_digit && !is_lower && !is_upper) {
            return false;
        }
    }
    return true;
}

const nlohmann::json &requireObjectMember(const nlohmann::json &object, std::string_view key,
                                          std::string_view context) {
    const auto it = object.find(key);
    if (it == object.end()) {
        throw std::runtime_error(std::string{context} + " requires " + std::string{key});
    }
    return it.value();
}

std::string requireStringMember(const nlohmann::json &object, std::string_view key, std::string_view context) {
    const auto &value = requireObjectMember(object, key, context);
    if (!value.is_string()) {
        throw std::runtime_error(std::string{context} + " requires string " + std::string{key});
    }
    return value.get<std::string>();
}

std::optional<std::string> optionalStringMember(const nlohmann::json &object, std::string_view key,
                                                std::string_view context) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    if (!it->is_string()) {
        throw std::runtime_error(std::string{context} + " optional " + std::string{key} +
                                 " must be a string");
    }
    return it->get<std::string>();
}

std::optional<int> optionalIntMember(const nlohmann::json &object, std::string_view key,
                                     std::string_view context) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return std::nullopt;
    }
    if (!it->is_number_integer()) {
        throw std::runtime_error(std::string{context} + " optional " + std::string{key} +
                                 " must be an integer");
    }
    return it->get<int>();
}

void validateEnvelope(const nlohmann::json &manifest) {
    if (!manifest.is_object()) {
        throw std::runtime_error("import manifest must be an object");
    }
    if (manifest.value("schema", std::string{}) != import_schema) {
        throw std::runtime_error("import manifest schema is not supported");
    }
    if (!manifest.contains("version") || !manifest.at("version").is_number_integer()) {
        throw std::runtime_error("import manifest requires numeric version");
    }
    const auto version = manifest.at("version").get<int>();
    if (version != supported_import_version) {
        throw std::runtime_error("import manifest version is not supported");
    }
}

void validateOutputFileReference(std::string_view file) {
    if (file.empty()) {
        throw std::runtime_error("import manifest outputs[].file must not be empty");
    }
    if (containsBackslash(file)) {
        throw std::runtime_error("import manifest outputs[].file must use forward slashes: " +
                                 std::string{file});
    }
    if (startsWithSlashRoot(file)) {
        throw std::runtime_error("import manifest outputs[].file must be relative: " + std::string{file});
    }
    if (hasScheme(file)) {
        throw std::runtime_error("import manifest outputs[].file must not use a URI scheme: " +
                                 std::string{file});
    }

    const std::filesystem::path path{std::string{file}};
    if (path.is_absolute() || path.has_root_name()) {
        throw std::runtime_error("import manifest outputs[].file must be relative: " + std::string{file});
    }
    if (path.filename().empty() || path == ".") {
        throw std::runtime_error("import manifest outputs[].file must name a file: " + std::string{file});
    }
    for (const auto &component : path) {
        if (component == "..") {
            throw std::runtime_error("import manifest outputs[].file must not escape delivery directory: " +
                                     std::string{file});
        }
    }
}

void validateOutputSchema(const std::string &schema, const std::optional<int> &version) {
    if (schema == "gltf" || schema == "png") {
        if (version) {
            throw std::runtime_error("import manifest output " + schema + " must not declare a version");
        }
        return;
    }
    if (schema == "pelican.transform_seq" || schema == "pelican.scene" ||
        schema == "pelican.layout" || schema == "pelican.atlas") {
        if (!version) {
            throw std::runtime_error("import manifest output " + schema + " requires version 1");
        }
        if (*version != 1) {
            throw std::runtime_error("import manifest output " + schema + " version is not supported");
        }
        return;
    }
    if (schema == "pelican.material") {
        if (!version) {
            throw std::runtime_error("import manifest output pelican.material requires version 1");
        }
        if (*version != 1) {
            throw std::runtime_error("import manifest output pelican.material version is not supported"
                                     " (expected=1, actual=" +
                                     std::to_string(*version) + ")");
        }
        return;
    }
    // pelican-import-tools の ktx2 レシピが出力する schema(KTX2 container
    // version 2)。エンジン側の読取サブセットは WP92 のとおり。
    if (schema == "khronos.ktx2") {
        if (!version) {
            throw std::runtime_error("import manifest output khronos.ktx2 requires version 2");
        }
        if (*version != 2) {
            throw std::runtime_error("import manifest output khronos.ktx2 version is not supported"
                                     " (expected=2, actual=" +
                                     std::to_string(*version) + ")");
        }
        return;
    }
    throw std::runtime_error("import manifest output schema is not supported: " + schema);
}

ImportManifestTool parseTool(const nlohmann::json &tool) {
    if (!tool.is_object()) {
        throw std::runtime_error("import manifest tool must be an object");
    }
    return ImportManifestTool{
        requireStringMember(tool, "name", "import manifest tool"),
        optionalStringMember(tool, "version", "import manifest tool"),
        optionalStringMember(tool, "dcc", "import manifest tool"),
    };
}

ImportManifestSource parseSource(const nlohmann::json &source) {
    if (!source.is_object()) {
        throw std::runtime_error("import manifest source must be an object");
    }
    std::string file;
    if (const auto found = source.find("file"); found != source.end()) {
        if (!found->is_string()) {
            throw std::runtime_error("import manifest source file must be a string");
        }
        file = found->get<std::string>();
    }
    bool has_directory = false;
    if (const auto found = source.find("directory"); found != source.end()) {
        if (!found->is_string() || found->get_ref<const std::string &>().empty()) {
            throw std::runtime_error("import manifest source directory must be a non-empty string");
        }
        has_directory = true;
    }
    bool has_files = false;
    if (const auto found = source.find("files"); found != source.end()) {
        if (!found->is_array() || found->empty()) {
            throw std::runtime_error("import manifest source files must be a non-empty string array");
        }
        for (const auto &entry : *found) {
            if (!entry.is_string() || entry.get_ref<const std::string &>().empty()) {
                throw std::runtime_error("import manifest source files must be a non-empty string array");
            }
        }
        has_files = true;
    }
    if (file.empty() && !has_directory && !has_files) {
        throw std::runtime_error("import manifest source requires file, directory, or non-empty files");
    }
    return ImportManifestSource{
        std::move(file),
        source,
    };
}

ImportManifestOutput parseOutput(const nlohmann::json &output, size_t index) {
    if (!output.is_object()) {
        throw std::runtime_error("import manifest outputs[" + std::to_string(index) +
                                 "] must be an object");
    }

    auto parsed = ImportManifestOutput{
        requireStringMember(output, "file", "import manifest output"),
        requireStringMember(output, "schema", "import manifest output"),
        optionalIntMember(output, "version", "import manifest output"),
        requireStringMember(output, "sha256", "import manifest output"),
    };

    validateOutputFileReference(parsed.file);
    validateOutputSchema(parsed.schema, parsed.version);
    if (!isHexSha256(parsed.sha256)) {
        throw std::runtime_error("import manifest output sha256 must be 64 hex characters: " +
                                 parsed.file);
    }
    return parsed;
}

} // namespace

ImportManifest parseImportManifestJson(const nlohmann::json &manifest) {
    validateEnvelope(manifest);

    const auto &outputs_json = requireObjectMember(manifest, "outputs", "import manifest");
    if (!outputs_json.is_array() || outputs_json.empty()) {
        throw std::runtime_error("import manifest requires non-empty outputs array");
    }

    ImportManifest parsed{
        parseTool(requireObjectMember(manifest, "tool", "import manifest")),
        parseSource(requireObjectMember(manifest, "source", "import manifest")),
        optionalStringMember(manifest, "created", "import manifest").value_or(std::string{}),
        {},
    };
    parsed.outputs.reserve(outputs_json.size());
    for (size_t i = 0; i < outputs_json.size(); ++i) {
        parsed.outputs.push_back(parseOutput(outputs_json.at(i), i));
    }
    return parsed;
}

} // namespace Pelican
