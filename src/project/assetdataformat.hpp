#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
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

struct AssetDataProceduralGeometryDeclaration {
    std::string name;
    std::string kind;
    std::array<float, 3> aabb_min{};
    std::array<float, 3> aabb_max{};
    nlohmann::ordered_json record = nlohmann::ordered_json::object();

    bool operator==(
        const AssetDataProceduralGeometryDeclaration &) const = default;
};

struct AssetDataFormatDocument {
    std::uint32_t version = 2;
    std::vector<AssetDataModelDeclaration> models;
    std::vector<AssetDataMaterialDeclaration> materials;
    std::vector<AssetDataProceduralGeometryDeclaration>
        procedural_geometry;
};

// pelican.asset_data is a strict, versioned project format. Only version 2 is
// read; any other version is pelican.asset_data.version_unsupported@1.
// Callers that need category-specific payloads such as sprite textures may
// continue reading those arrays after this envelope validation succeeds.
AssetDataFormatDocument
parseAssetDataFormatJson(const nlohmann::json &document);

} // namespace Pelican
