#include "atlasasset.hpp"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace Pelican::asset {
namespace {

void requireClosed(const nlohmann::json &object,
                   std::initializer_list<std::string_view> fields,
                   std::string_view context) {
    if (!object.is_object()) throw std::runtime_error(std::string{context} + " must be an object");
    for (const auto &[name, value] : object.items()) {
        (void)value;
        if (std::find(fields.begin(), fields.end(), name) == fields.end())
            throw std::runtime_error(std::string{context} + " has unknown field: " + name);
    }
}

std::int32_t positiveI32(const nlohmann::json &value, std::string_view field) {
    if (!value.is_number_integer())
        throw std::runtime_error("pelican.atlas v1 " + std::string{field} + " must be an integer");
    const auto parsed = value.get<std::int64_t>();
    if (parsed <= 0 || parsed > std::numeric_limits<std::int32_t>::max())
        throw std::runtime_error("pelican.atlas v1 " + std::string{field} + " is outside int32");
    return static_cast<std::int32_t>(parsed);
}

AtlasRect parseRect(const nlohmann::json &value, const std::string &name) {
    if (!value.is_array() || value.size() != 4)
        throw std::runtime_error("pelican.atlas v1 sprite rect must have four integers: " + name);
    std::int32_t parts[4]{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!value[i].is_number_integer())
            throw std::runtime_error("pelican.atlas v1 sprite rect must contain integers: " + name);
        const auto parsed = value[i].get<std::int64_t>();
        if (parsed < 0 || parsed > std::numeric_limits<std::int32_t>::max())
            throw std::runtime_error("pelican.atlas v1 sprite rect is outside int32: " + name);
        parts[i] = static_cast<std::int32_t>(parsed);
    }
    return {parts[0], parts[1], parts[2], parts[3]};
}

SamplerKey parseSampler(const nlohmann::json &declaration) {
    const auto sampler = declaration.value("sampler", std::string{"linear"});
    if (sampler == "nearest") return SamplerKey::nearest;
    if (sampler == "linear") return SamplerKey::linear;
    throw std::runtime_error("sprite asset sampler must be nearest or linear: " + sampler);
}

} // namespace

AtlasAsset parseAtlasAssetV1(const nlohmann::json &json,
                             const std::filesystem::path &source_path) {
    requireClosed(json, {"schema", "version", "pages", "sprites"}, "pelican.atlas v1");
    if (json.value("schema", "") != "pelican.atlas" || json.value("version", 0) != 1)
        throw std::runtime_error("expected pelican.atlas version 1: " + source_path.string());
    if (!json.contains("pages") || !json.at("pages").is_array() || json.at("pages").empty())
        throw std::runtime_error("pelican.atlas v1 pages must be a non-empty array: " + source_path.string());
    if (json.at("pages").size() > std::numeric_limits<std::uint16_t>::max())
        throw std::runtime_error("limit_exceeded: pelican.atlas page count exceeds uint16");

    AtlasAsset result{.source_path = source_path};
    for (const auto &page : json.at("pages")) {
        requireClosed(page, {"image", "size"}, "pelican.atlas v1 page");
        if (!page.contains("image") || !page.at("image").is_string() ||
            page.at("image").get_ref<const std::string &>().empty())
            throw std::runtime_error("pelican.atlas v1 page image is required");
        const auto image = std::filesystem::path{page.at("image").get<std::string>()};
        if (image.is_absolute() || image.has_root_name())
            throw std::runtime_error("pelican.atlas v1 page image must be relative");
        for (const auto &part : image)
            if (part == "..") throw std::runtime_error("pelican.atlas v1 page image must not traverse parents");
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
        if (page >= result.pages.size())
            throw std::runtime_error("pelican.atlas v1 sprite page is out of range: " + name);
        const auto rect = parseRect(sprite.at("rect"), name);
        const auto page_size = result.pages[page].size;
        if (!rect.ordered() || rect.left == rect.right || rect.top == rect.bottom ||
            rect.right > page_size.width || rect.bottom > page_size.height)
            throw std::runtime_error("pelican.atlas v1 sprite rect is outside its page: " + name);
        result.sprites.push_back({name, static_cast<std::uint16_t>(page), rect});
    }
    std::sort(result.sprites.begin(), result.sprites.end(),
              [](const auto &a, const auto &b) { return a.name < b.name; });
    return result;
}

const AtlasSprite &findAtlasSprite(const AtlasAsset &atlas, std::string_view name) {
    const auto found = std::lower_bound(atlas.sprites.begin(), atlas.sprites.end(), name,
                                        [](const AtlasSprite &sprite, std::string_view key) {
                                            return sprite.name < key;
                                        });
    if (found == atlas.sprites.end() || found->name != name)
        throw std::runtime_error("pelican.atlas v1 sprite not found: " + std::string{name});
    return *found;
}

std::vector<SpriteAssetDeclaration> parseSpriteAssetDeclarations(const nlohmann::json &asset_data) {
    if (!asset_data.is_object()) throw std::runtime_error("asset_data_json must be an object");
    const auto found = asset_data.find("textures");
    if (found == asset_data.end()) return {};
    if (!found->is_array()) throw std::runtime_error("asset_data_json textures must be an array");
    std::vector<SpriteAssetDeclaration> result;
    std::set<std::string, std::less<>> ids;
    for (const auto &value : *found) {
        requireClosed(value, {"name", "path", "sampler"}, "sprite asset declaration");
        if (!value.contains("name") || !value.at("name").is_string() ||
            value.at("name").get_ref<const std::string &>().empty())
            throw std::runtime_error("sprite asset declaration requires non-empty name");
        if (!value.contains("path") || !value.at("path").is_string() ||
            value.at("path").get_ref<const std::string &>().empty())
            throw std::runtime_error("sprite asset declaration requires non-empty path");
        SpriteAssetDeclaration declaration{value.at("name").get<std::string>(),
                                           value.at("path").get<std::string>(), parseSampler(value)};
        if (!ids.insert(declaration.id).second)
            throw std::runtime_error("duplicate sprite asset declaration: " + declaration.id);
        result.push_back(std::move(declaration));
    }
    return result;
}

SpriteAssetReference parseSpriteAssetReference(std::string_view reference) {
    if (reference.empty()) throw std::runtime_error("sprite_view texture must not be empty");
    if (reference.starts_with("snapshot:"))
        throw std::runtime_error("sprite_view snapshot: references are reserved for a future version");
    const auto marker = reference.find('#');
    if (marker == std::string_view::npos)
        return {std::string{reference}, {}, false};
    if (reference.find('#', marker + 1) != std::string_view::npos)
        throw std::runtime_error("sprite_view texture contains multiple fragments");
    constexpr std::string_view prefix = "#sprite/";
    if (reference.substr(marker) != prefix && !reference.substr(marker).starts_with(prefix))
        throw std::runtime_error("sprite_view texture fragment must use #sprite/name");
    const auto asset_id = reference.substr(0, marker);
    const auto sprite_name = reference.substr(marker + prefix.size());
    if (asset_id.empty() || sprite_name.empty())
        throw std::runtime_error("sprite_view texture requires asset id and sprite fragment name");
    return {std::string{asset_id}, std::string{sprite_name}, true};
}

SpriteAssetCatalog::SpriteAssetCatalog(const nlohmann::json &asset_data)
    : declarations{parseSpriteAssetDeclarations(asset_data)} {}

const SpriteAssetDeclaration &SpriteAssetCatalog::declaration(std::string_view id) const {
    const auto found = std::find_if(declarations.begin(), declarations.end(),
                                    [&](const auto &value) { return value.id == id; });
    if (found == declarations.end())
        throw std::runtime_error("sprite asset declaration not found: " + std::string{id});
    return *found;
}

ResolvedSpriteAsset SpriteAssetCatalog::resolve(
    std::string_view reference,
    const std::function<std::filesystem::path(std::string_view)> &resolve_path,
    const std::function<AtlasAsset(const std::filesystem::path &)> &load_atlas) const {
    const auto parsed = parseSpriteAssetReference(reference);
    const auto &decl = declaration(parsed.asset_id);
    const auto path = resolve_path(decl.path);
    if (!parsed.atlas_fragment) return {.declaration = decl, .source_path = path};
    const auto atlas = load_atlas(path);
    const auto &sprite = findAtlasSprite(atlas, parsed.sprite_name);
    return {.declaration = decl, .source_path = path, .page = sprite.page,
            .rect = sprite.rect, .page_size = atlas.pages.at(sprite.page).size};
}

} // namespace Pelican::asset
