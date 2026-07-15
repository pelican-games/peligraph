#include "spritescene.hpp"

#include "atlasassetresource.hpp"
#include "camera.hpp"
#include "../asset/atlasasset.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/imageloader.hpp"
#include "../loader/pathresolver.hpp"
#include "../log.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
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

sprite::SortPolicy spriteSortPolicy(CameraSpriteSortPolicy policy) {
    switch (policy) {
    case CameraSpriteSortPolicy::z: return sprite::SortPolicy::z;
    case CameraSpriteSortPolicy::y_down: return sprite::SortPolicy::y_down;
    case CameraSpriteSortPolicy::declaration: return sprite::SortPolicy::declaration;
    }
    return sprite::SortPolicy::z;
}

const char *spriteSortPolicyName(sprite::SortPolicy policy) {
    switch (policy) {
    case sprite::SortPolicy::z: return "z";
    case sprite::SortPolicy::y_down: return "y_down";
    case sprite::SortPolicy::declaration: return "declaration";
    }
    return "z";
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

    ProjectBasicConfig &project_config;
    PathResolver &path_resolver;
    AtlasAssetResource &atlas;
    Camera &camera;
    asset::SpriteAssetCatalog catalog;
    std::map<std::string, ResolvedTexture, std::less<>> textures;
    std::map<EntityId, std::uint64_t> declaration_sequences;
    sprite::DeclarationSequenceAllocator declaration_allocator;
    sprite::SpriteFrame current_frame;
    sprite::SortPolicy sort_policy = sprite::SortPolicy::z;
    sprite::PixelContract pixel_contract;
    std::map<sprite::PixelSnapReason, std::size_t> pixel_reason_counts;
    std::set<std::string, std::less<>> warned_linear_assets;
    std::set<std::string, std::less<>> warned_contract_failures;

    State()
        : project_config{GET_MODULE(ProjectBasicConfig)},
          path_resolver{GET_MODULE(PathResolver)},
          atlas{GET_MODULE(AtlasAssetResource)},
          camera{GET_MODULE(Camera)},
          catalog{nlohmann::json::parse(project_config.assetDataJson())} {}

    ResolvedTexture resolve(std::string_view reference) {
        if (const auto found = textures.find(reference); found != textures.end()) return found->second;
        const auto parsed = asset::parseSpriteAssetReference(reference);
        const auto &declaration = catalog.declaration(parsed.asset_id);
        const auto source_path = path_resolver.resolveExistingFile(declaration.path);

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
        const auto page = atlas.registerPage(page_source);
        ResolvedTexture result{stableAssetId(declaration.id), page, declaration.sampler, rect, extent};
        textures.emplace(reference, result);
        return result;
    }

    ResolvedTexture prepared(std::string_view reference) const {
        const auto found = textures.find(reference);
        if (found == textures.end()) {
            throw std::logic_error("sprite asset was not prepared on the ECS owner thread: " +
                                   std::string{reference});
        }
        return found->second;
    }
};

SpriteScene::SpriteScene() : state{std::make_unique<State>()} {}
SpriteScene::~SpriteScene() = default;

void SpriteScene::prepareAssets(std::span<const SpriteSceneItem> items) {
    for (const auto &item : items) (void)state->resolve(item.view->texture);
}

void SpriteScene::rebuild(std::span<const SpriteSceneItem> items) {
    std::vector<sprite::SpriteCommand> commands;
    commands.reserve(items.size());
    const auto view = state->camera.getViewMatrix();
    const auto camera_policy = state->camera.getSpritePolicy();
    state->sort_policy = spriteSortPolicy(camera_policy.sort);
    const auto ppu = state->project_config.spritePixelsPerUnit();
    for (const auto &item : items) {
        const auto texture = state->prepared(item.view->texture);
        const float width = item.view->has_explicit_size ? item.view->size.x
                                                         : float(texture.rect.right - texture.rect.left) / ppu;
        const float height = item.view->has_explicit_size ? item.view->size.y
                                                          : float(texture.rect.bottom - texture.rect.top) / ppu;
        if (camera_policy.pixel_perfect == CameraPixelPerfectMode::strict &&
            texture.sampler != sprite::SamplerKey::nearest &&
            state->warned_linear_assets.insert(item.view->texture).second) {
            LOG_WARNING(logger,
                        "strict pixel-perfect sprite '{}' uses a non-nearest asset sampler; "
                        "this sprite is downgraded and remains observable in get_status.sprite",
                        item.view->texture);
        }
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
        command.source_texel_extent = {
            static_cast<std::uint32_t>(texture.rect.right - texture.rect.left),
            static_cast<std::uint32_t>(texture.rect.bottom - texture.rect.top),
        };
        command.canvas_bounds = commandBounds(world, *item.view);
        commands.push_back(std::move(command));
    }
    state->current_frame = sprite::buildFrame(std::move(commands),
                                               {-1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f},
                                               state->sort_policy);
    updatePixelPolicy(state->camera.viewportWidth(), state->camera.viewportHeight());
}

void SpriteScene::updatePixelPolicy(std::uint32_t viewport_width, std::uint32_t viewport_height) {
    const auto &camera = state->camera;
    const auto projection = camera.getProjectionSpec();
    const auto policy = camera.getSpritePolicy();
    state->pixel_contract = sprite::evaluatePixelContract(
        policy.pixel_perfect == CameraPixelPerfectMode::strict,
        projection.kind == CameraProjectionKind::Orthographic,
        projection.xmag, projection.ymag,
        state->project_config.spritePixelsPerUnit(),
        viewport_width, viewport_height);
    state->pixel_reason_counts.clear();

    if (state->pixel_contract.requested && !state->pixel_contract.active) {
        const auto failure = std::string{sprite::pixelContractFailureName(state->pixel_contract.failure)};
        const auto camera_name = camera.activeCameraName().empty() ? std::string{"<default>"}
                                                                   : camera.activeCameraName();
        if (state->warned_contract_failures.insert(camera_name + ":" + failure).second) {
            LOG_WARNING(logger,
                        "strict pixel-perfect camera '{}' is inactive for this frame: {} "
                        "(full framebuffer viewport, no letterbox fallback)",
                        camera_name, failure);
        }
    }

    const auto view = camera.getViewMatrix();
    for (auto &chunk : state->current_frame.chunks) {
        for (auto &command : chunk.commands) {
            const auto world = glm::make_mat4(command.world_transform.data());
            const auto view_world = view * world;
            const sprite::StrictSpriteInput input{
                .sampler = command.batch.sampler,
                .billboard = command.billboard,
                .view_basis_x = {view_world[0].x, view_world[0].y, view_world[0].z},
                .view_basis_y = {view_world[1].x, view_world[1].y, view_world[1].z},
                .source_width = command.source_texel_extent[0],
                .source_height = command.source_texel_extent[1],
            };
            command.pixel_snap = sprite::classifyStrictSprite(state->pixel_contract, input);
            ++state->pixel_reason_counts[command.pixel_snap];
        }
    }
}

void SpriteScene::clear() {
    state->current_frame = {};
    state->pixel_reason_counts.clear();
}
const sprite::SpriteFrame &SpriteScene::frame() const { return state->current_frame; }
std::size_t SpriteScene::commandCountForTesting() const { return state->current_frame.visible_count; }

nlohmann::json SpriteScene::statusJson() const {
    nlohmann::json downgrades = nlohmann::json::object();
    std::size_t eligible = 0;
    for (const auto &[reason, count] : state->pixel_reason_counts) {
        if (reason == sprite::PixelSnapReason::eligible) eligible += count;
        else if (reason != sprite::PixelSnapReason::not_requested)
            downgrades[sprite::pixelSnapReasonName(reason)] = count;
    }
    const auto &contract = state->pixel_contract;
    return nlohmann::json{
        {"enabled", true},
        {"sort", spriteSortPolicyName(state->sort_policy)},
        {"pixels_per_unit", contract.pixels_per_unit},
        {"logical_count", state->current_frame.logical_count},
        {"visible_count", state->current_frame.visible_count},
        {"chunks", state->current_frame.chunks.size()},
        {"pixel_perfect",
         {{"requested", contract.requested},
          {"active", contract.active},
          {"method", "render_only_quantization"},
          {"content_viewport",
           {{"x", 0}, {"y", 0}, {"width", contract.viewport_width},
            {"height", contract.viewport_height}}},
          {"pixel_center", nlohmann::json::array({0.5, 0.5})},
          {"integer_tolerance", sprite::pixelContractTolerance},
          {"world_units_per_pixel",
           nlohmann::json::array({contract.world_units_per_pixel_x,
                                  contract.world_units_per_pixel_y})},
          {"zoom", nlohmann::json::array({contract.zoom_x, contract.zoom_y})},
          {"integer_zoom", contract.integer_zoom},
          {"failure", contract.active ? nlohmann::json(nullptr)
                                        : nlohmann::json(sprite::pixelContractFailureName(contract.failure))},
          {"eligible", eligible},
          {"downgraded", contract.requested ? state->current_frame.visible_count - eligible : 0},
          {"downgrades", std::move(downgrades)}}},
    };
}

} // namespace Pelican
