#include "spritescene.hpp"

#include "atlasassetresource.hpp"
#include "camera.hpp"
#include "../asset/atlasasset.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/imageloader.hpp"
#include "../loader/pathresolver.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace Pelican {
namespace {

std::uint32_t stableAssetId(std::string_view value) {
    std::uint32_t hash = UINT32_C(2166136261);
    for (const auto ch : value) {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= UINT32_C(16777619);
    }
    return hash;
}

nlohmann::json loadJson(const std::filesystem::path &path) {
    std::ifstream input{path};
    if (!input) throw std::runtime_error("failed to open sprite asset: " + path.string());
    return nlohmann::json::parse(input);
}

std::array<float, 16> matrixArray(const glm::mat4 &matrix) {
    std::array<float, 16> result{};
    const auto *values = reinterpret_cast<const float *>(&matrix);
    std::copy_n(values, result.size(), result.data());
    return result;
}

sprite::Bounds2 commandBounds(const glm::mat4 &world, const SpriteViewComponent &view) {
    sprite::Bounds2 result{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                           std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const glm::vec2 corner : {glm::vec2{0, 0}, glm::vec2{1, 0}, glm::vec2{1, 1}, glm::vec2{0, 1}}) {
        const auto p = world * glm::vec4{corner.x - view.pivot.x, corner.y - view.pivot.y, 0.0f, 1.0f};
        result.min_x = std::min(result.min_x, p.x);
        result.min_y = std::min(result.min_y, p.y);
        result.max_x = std::max(result.max_x, p.x);
        result.max_y = std::max(result.max_y, p.y);
    }
    return result;
}

} // namespace

struct SpriteScene::State {
    struct ResolvedTexture {
        std::uint32_t asset_id = 0;
        std::uint16_t page = 0;
        sprite::SamplerKey sampler = sprite::SamplerKey::linear;
        asset::AtlasRect rect{};
        asset::AtlasExtent extent{};
    };

    asset::SpriteAssetCatalog catalog;
    std::map<std::string, ResolvedTexture, std::less<>> textures;
    std::map<EntityId, std::uint64_t> declaration_sequences;
    sprite::DeclarationSequenceAllocator declaration_allocator;
    sprite::SpriteFrame current_frame;

    State() : catalog{nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson())} {}

    ResolvedTexture resolve(std::string_view reference) {
        if (const auto found = textures.find(reference); found != textures.end()) return found->second;
        const auto parsed = asset::parseSpriteAssetReference(reference);
        const auto &declaration = catalog.declaration(parsed.asset_id);
        const auto source_path = GET_MODULE(PathResolver).resolveExistingFile(declaration.path);

        AtlasPageSource page_source;
        asset::AtlasRect rect;
        asset::AtlasExtent extent;
        if (parsed.atlas_fragment) {
            const auto atlas = asset::parseAtlasAssetV1(loadJson(source_path), source_path);
            const auto &entry = asset::findAtlasSprite(atlas, parsed.sprite_name);
            const auto &page = atlas.pages.at(entry.page);
            page_source = AtlasPageSource{.stable_name = "atlas:" + source_path.generic_string() +
                                                         "/page:" + std::to_string(entry.page),
                                          .image_path = page.image_path, .size = page.size};
            rect = entry.rect;
            extent = page.size;
        } else {
            const auto image = loadImageFile(source_path);
            extent = {static_cast<std::int32_t>(image.width), static_cast<std::int32_t>(image.height)};
            rect = {0, 0, extent.width, extent.height};
            page_source = AtlasPageSource{.stable_name = "image:" + source_path.generic_string(),
                                          .image_path = source_path, .size = extent};
        }
        const auto page = GET_MODULE(AtlasAssetResource).registerPage(page_source);
        ResolvedTexture result{stableAssetId(declaration.id), page, declaration.sampler, rect, extent};
        textures.emplace(reference, result);
        return result;
    }
};

SpriteScene::SpriteScene() : state{std::make_unique<State>()} {}
SpriteScene::~SpriteScene() = default;

void SpriteScene::rebuild(std::span<const SpriteSceneItem> items) {
    std::vector<sprite::SpriteCommand> commands;
    commands.reserve(items.size());
    const auto view = GET_MODULE(Camera).getViewMatrix();
    for (const auto &item : items) {
        const auto texture = state->resolve(item.view->texture);
        const float width = item.view->has_explicit_size ? item.view->size.x
                                                         : float(texture.rect.right - texture.rect.left) / 100.0f;
        const float height = item.view->has_explicit_size ? item.view->size.y
                                                          : float(texture.rect.bottom - texture.rect.top) / 100.0f;
        const auto &t = *item.transform;
        glm::mat4 world = glm::translate(glm::mat4{1.0f}, t.pos) * glm::mat4_cast(t.rotation) *
                          glm::scale(glm::mat4{1.0f}, t.scale * glm::vec3{width, height, 1.0f});
        const glm::vec4 pivot_world = world * glm::vec4{0, 0, 0, 1};
        const glm::vec4 view_position = view * pivot_world;

        auto [sequence, inserted] = state->declaration_sequences.emplace(item.entity, 0);
        if (inserted) sequence->second = state->declaration_allocator.issue();
        const float left = float(texture.rect.left) / float(texture.extent.width);
        const float right = float(texture.rect.right) / float(texture.extent.width);
        const float top = float(texture.rect.top) / float(texture.extent.height);
        const float bottom = float(texture.rect.bottom) / float(texture.extent.height);

        sprite::SpriteCommand command;
        command.world_transform = matrixArray(world);
        command.pivot = {item.view->pivot.x, item.view->pivot.y};
        command.uv_rect = {item.view->flip_x ? right : left, item.view->flip_y ? bottom : top,
                           item.view->flip_x ? left : right, item.view->flip_y ? top : bottom};
        command.color = {item.view->color.x, item.view->color.y, item.view->color.z, item.view->color.w};
        command.batch = {texture.asset_id, texture.page, texture.sampler};
        command.layer = item.view->layer;
        command.view_depth = -view_position.z;
        command.pivot_world_y = pivot_world.y;
        command.declaration_seq = sequence->second;
        command.source_entity = item.entity;
        command.source_ordinal = 0;
        command.billboard = item.view->billboard;
        command.canvas_bounds = commandBounds(world, *item.view);
        commands.push_back(std::move(command));
    }
    state->current_frame = sprite::buildFrame(std::move(commands),
                                               {-1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f},
                                               sprite::SortPolicy::z);
}

void SpriteScene::clear() { state->current_frame = {}; }
const sprite::SpriteFrame &SpriteScene::frame() const { return state->current_frame; }
std::size_t SpriteScene::commandCountForTesting() const { return state->current_frame.visible_count; }

} // namespace Pelican
