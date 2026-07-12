#pragma once

#include <sprite/sampler.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican::asset {

using SamplerKey = sprite::SamplerKey;

struct AtlasExtent {
    std::int32_t width = 0;
    std::int32_t height = 0;
    auto operator<=>(const AtlasExtent &) const = default;
};

struct AtlasRect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    auto operator<=>(const AtlasRect &) const = default;
    bool ordered() const noexcept { return left <= right && top <= bottom; }
};

struct AtlasPage {
    std::filesystem::path image_path;
    AtlasExtent size{};
};

struct AtlasSprite {
    std::string name;
    std::uint16_t page = 0;
    AtlasRect rect{};
};

// Consumer-neutral CPU asset. UI and world sprites adapt this document to
// their own command/vertex ABIs; neither consumer owns the parser.
struct AtlasAsset {
    std::filesystem::path source_path;
    std::vector<AtlasPage> pages;
    std::vector<AtlasSprite> sprites;
};

AtlasAsset parseAtlasAssetV1(const nlohmann::json &json,
                             const std::filesystem::path &source_path);
const AtlasSprite &findAtlasSprite(const AtlasAsset &atlas, std::string_view name);

struct SpriteAssetDeclaration {
    std::string id;
    std::string path;
    SamplerKey sampler = SamplerKey::linear;
};

struct SpriteAssetReference {
    std::string asset_id;
    std::string sprite_name;
    bool atlas_fragment = false;
};

struct ResolvedSpriteAsset {
    SpriteAssetDeclaration declaration;
    std::filesystem::path source_path;
    std::uint16_t page = 0;
    AtlasRect rect{};
    AtlasExtent page_size{};
};

std::vector<SpriteAssetDeclaration> parseSpriteAssetDeclarations(const nlohmann::json &asset_data);
SpriteAssetReference parseSpriteAssetReference(std::string_view reference);

class SpriteAssetCatalog {
    std::vector<SpriteAssetDeclaration> declarations;

  public:
    explicit SpriteAssetCatalog(const nlohmann::json &asset_data);
    const SpriteAssetDeclaration &declaration(std::string_view id) const;
    ResolvedSpriteAsset resolve(
        std::string_view reference,
        const std::function<std::filesystem::path(std::string_view)> &resolve_path,
        const std::function<AtlasAsset(const std::filesystem::path &)> &load_atlas) const;
};

} // namespace Pelican::asset
