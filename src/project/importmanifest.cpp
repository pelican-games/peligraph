#include "importmanifest.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

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
    if (schema == "gltf") {
        return;
    }
    if (schema == "pelican.transform_seq") {
        if (version && *version != 1) {
            throw std::runtime_error("import manifest output pelican.transform_seq version is not supported");
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
    return ImportManifestSource{
        requireStringMember(source, "file", "import manifest source"),
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
        requireStringMember(manifest, "created", "import manifest"),
        {},
    };
    parsed.outputs.reserve(outputs_json.size());
    for (size_t i = 0; i < outputs_json.size(); ++i) {
        parsed.outputs.push_back(parseOutput(outputs_json.at(i), i));
    }
    return parsed;
}

} // namespace Pelican
