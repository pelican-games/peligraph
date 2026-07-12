#include "atlas.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace Pelican::ui {
namespace {

std::int32_t positiveI32(const nlohmann::json &value, std::string_view field) {
    if (!value.is_number_integer()) throw std::runtime_error("pelican.atlas v1 " + std::string{field} + " must be an integer");
    const auto parsed = value.get<std::int64_t>();
    if (parsed <= 0 || parsed > std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error("pelican.atlas v1 " + std::string{field} + " is outside int32");
    return static_cast<std::int32_t>(parsed);
}

RectI parseRect(const nlohmann::json &value, const std::string &name) {
    if (!value.is_array() || value.size() != 4) throw std::runtime_error("pelican.atlas v1 sprite rect must have four integers: " + name);
    std::int32_t parts[4]{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!value[i].is_number_integer()) throw std::runtime_error("pelican.atlas v1 sprite rect must contain integers: " + name);
        const auto parsed = value[i].get<std::int64_t>();
        if (parsed < 0 || parsed > std::numeric_limits<std::int32_t>::max())
            throw std::runtime_error("pelican.atlas v1 sprite rect is outside int32: " + name);
        parts[i] = static_cast<std::int32_t>(parsed);
    }
    return {parts[0], parts[1], parts[2], parts[3]};
}

void requireClosed(const nlohmann::json &object, std::initializer_list<std::string_view> fields,
                   std::string_view context) {
    if (!object.is_object()) throw std::runtime_error(std::string{context} + " must be an object");
    for (const auto &[name, value] : object.items()) {
        (void)value;
        if (std::find(fields.begin(), fields.end(), name) == fields.end())
            throw std::runtime_error(std::string{context} + " has unknown field: " + name);
    }
}

} // namespace

AtlasDocument parseAtlasV1(const nlohmann::json &json, const std::filesystem::path &source_path) {
    requireClosed(json, {"schema", "version", "pages", "sprites"}, "pelican.atlas v1");
    if (json.value("schema", "") != "pelican.atlas" || json.value("version", 0) != 1)
        throw std::runtime_error("expected pelican.atlas version 1: " + source_path.string());
    if (!json.contains("pages") || !json.at("pages").is_array() || json.at("pages").empty())
        throw std::runtime_error("pelican.atlas v1 pages must be a non-empty array: " + source_path.string());
    if (json.at("pages").size() > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error("limit_exceeded: pelican.atlas page count exceeds uint16");

    AtlasDocument result{.source_path = source_path};
    for (std::size_t i = 0; i < json.at("pages").size(); ++i) {
        const auto &page = json.at("pages")[i];
        requireClosed(page, {"image", "size"}, "pelican.atlas v1 page");
        if (!page.contains("image") || !page.at("image").is_string() || page.at("image").get_ref<const std::string &>().empty())
            throw std::runtime_error("pelican.atlas v1 page image is required");
        const auto image = std::filesystem::path{page.at("image").get<std::string>()};
        if (image.is_absolute() || image.has_root_name()) throw std::runtime_error("pelican.atlas v1 page image must be relative");
        for (const auto &part : image) if (part == "..") throw std::runtime_error("pelican.atlas v1 page image must not traverse parents");
        if (!page.contains("size") || !page.at("size").is_array() || page.at("size").size() != 2)
            throw std::runtime_error("pelican.atlas v1 page size must have two integers");
        result.pages.push_back({source_path.parent_path() / image,
                                {positiveI32(page.at("size")[0], "page width"),
                                 positiveI32(page.at("size")[1], "page height")}});
    }
    if (!json.contains("sprites") || !json.at("sprites").is_object())
        throw std::runtime_error("pelican.atlas v1 sprites must be an object");
    for (const auto &[name, sprite] : json.at("sprites").items()) {
        requireClosed(sprite, {"page", "rect"}, "pelican.atlas v1 sprite " + name);
        if (name.empty() || !sprite.contains("page") || !sprite.at("page").is_number_unsigned())
            throw std::runtime_error("pelican.atlas v1 sprite page is required: " + name);
        const auto page = sprite.at("page").get<std::uint64_t>();
        if (page >= result.pages.size()) throw std::runtime_error("pelican.atlas v1 sprite page is out of range: " + name);
        const auto rect = parseRect(sprite.at("rect"), name);
        const auto page_size = result.pages[page].size;
        if (!rect.ordered() || rect.left == rect.right || rect.top == rect.bottom ||
            rect.right > page_size.x || rect.bottom > page_size.y)
            throw std::runtime_error("pelican.atlas v1 sprite rect is outside its page: " + name);
        result.sprites.push_back({name, static_cast<std::uint16_t>(page), rect});
    }
    std::sort(result.sprites.begin(), result.sprites.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
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
