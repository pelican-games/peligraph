#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Pelican {

struct ImportManifestTool {
    std::string name;
    std::optional<std::string> version;
    std::optional<std::string> dcc;
};

struct ImportManifestSource {
    std::string file;
    nlohmann::json metadata;
};

struct ImportManifestOutput {
    std::string file;
    std::string schema;
    std::optional<int> version;
    std::string sha256;
};

struct ImportManifest {
    ImportManifestTool tool;
    ImportManifestSource source;
    std::string created;
    std::vector<ImportManifestOutput> outputs;
};

ImportManifest parseImportManifestJson(const nlohmann::json &manifest);

} // namespace Pelican
