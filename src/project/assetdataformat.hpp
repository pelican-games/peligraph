#pragma once

#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Pelican {

struct AssetDataModelDeclaration {
    std::string name;
    std::string path;
    std::optional<std::string> material_bindings;
};

struct AssetDataMaterialDeclaration {
    std::string path;
};

struct AssetDataFormatDocument {
    std::vector<AssetDataModelDeclaration> models;
    std::vector<AssetDataMaterialDeclaration> materials;
};

// pelican.asset_data is a strict, single-current-version project format.
// Callers that need category-specific payloads such as sprite textures may
// continue reading those arrays after this envelope validation succeeds.
AssetDataFormatDocument
parseAssetDataFormatJson(const nlohmann::json &document);

} // namespace Pelican
