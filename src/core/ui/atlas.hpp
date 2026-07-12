#pragma once

#include "../asset/atlasasset.hpp"
#include "types.hpp"

#include <cstdint>
#include <filesystem>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

namespace Pelican::ui {

struct AtlasPage {
    std::filesystem::path image_path;
    PointI size{};
};

struct AtlasSprite {
    std::string name;
    std::uint16_t page = 0;
    RectI rect{};
};

struct AtlasDocument {
    std::filesystem::path source_path;
    std::vector<AtlasPage> pages;
    std::vector<AtlasSprite> sprites;
};

AtlasDocument parseAtlasV1(const nlohmann::json &json, const std::filesystem::path &source_path);
const AtlasSprite &findAtlasSprite(const AtlasDocument &atlas, std::string_view name);

} // namespace Pelican::ui
