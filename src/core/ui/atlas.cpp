#include "atlas.hpp"

#include <nlohmann/json.hpp>

namespace Pelican::ui {
AtlasDocument parseAtlasV1(const nlohmann::json &json, const std::filesystem::path &source_path) {
    const auto neutral = asset::parseAtlasAssetV1(json, source_path);
    AtlasDocument result{.source_path = neutral.source_path};
    for (const auto &page : neutral.pages)
        result.pages.push_back({page.image_path, {page.size.width, page.size.height}});
    for (const auto &sprite : neutral.sprites)
        result.sprites.push_back({sprite.name, sprite.page,
                                  {sprite.rect.left, sprite.rect.top, sprite.rect.right, sprite.rect.bottom}});
    return result;
}

const AtlasSprite &findAtlasSprite(const AtlasDocument &atlas, std::string_view name) {
    const auto found = std::lower_bound(atlas.sprites.begin(), atlas.sprites.end(), name,
                                        [](const AtlasSprite &sprite, std::string_view key) { return sprite.name < key; });
    if (found == atlas.sprites.end() || found->name != name)
        throw std::runtime_error("pelican.atlas v1 sprite not found: " + std::string{name});
    return *found;
}

} // namespace Pelican::ui
