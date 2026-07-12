#include "texturereloadhandler.hpp"

#include "../loader/imageloader.hpp"
#include "../log.hpp"
#include "../vkcore/deletionqueue.hpp"
#include "materialcontainer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace Pelican {
namespace {

constexpr std::string_view textureTable = "texture";
constexpr std::string_view materialDescriptorTable = "material-descriptor";

watch::AssetKey materialSource(GlobalMaterialId material) {
    return {"@runtime/material/" + std::to_string(material.value), {}};
}

} // namespace

struct TextureReloadHandler::TexturePayload {
    watch::AssetKey key;
    GlobalTextureId texture;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ImagePixelFormat format = ImagePixelFormat::Rgba8Unorm;
    std::uint32_t mip_levels = 1;
    std::size_t bytes = 0;
    bool in_place = false;
    mutable std::optional<MaterialContainer::InternalTextureResource> replacement;
    mutable std::vector<MaterialContainer::StagedMaterialDescriptor> descriptors;
};

struct TextureReloadHandler::MaterialDescriptorPayload {
    GlobalMaterialId material;
};

struct TextureReloadHandler::TrackedTexture {
    std::filesystem::path path;
    GlobalTextureId texture;
    watch::LogicalResourceRef resource;
    std::shared_ptr<const void> live_payload;
};

struct TextureReloadHandler::TrackedMaterial {
    watch::LogicalResourceRef resource;
    std::shared_ptr<const void> live_payload;
};

struct TextureReloadHandler::PendingReload {
    std::shared_ptr<LoadedImage> decoded;
};

TextureReloadHandler::TextureReloadHandler(MaterialContainer &materials,
                                           watch::ReloadCoordinator &coordinator)
    : materials_{materials}, coordinator_{coordinator} {}

TextureReloadHandler::~TextureReloadHandler() = default;

std::vector<watch::AssetKey>
TextureReloadHandler::materialDependencies(GlobalMaterialId material) const {
    std::vector<watch::AssetKey> dependencies;
    const auto &info = materials_.materials.get(material);
    for (const auto &binding : info.texture_bindings) {
        for (const auto &[key, tracked] : textures_) {
            if (tracked.texture == binding.texture) dependencies.push_back(key);
        }
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    return dependencies;
}

void TextureReloadHandler::refreshMaterial(GlobalMaterialId material) {
    auto dependencies = materialDependencies(material);
    if (dependencies.empty()) return;

    auto payload = std::make_shared<MaterialDescriptorPayload>(
        MaterialDescriptorPayload{material});
    const auto found = material_resources_.find(material.value);
    if (found == material_resources_.end()) {
        auto resource = coordinator_.registry().declareResource(
            std::string{materialDescriptorTable}, materialSource(material), payload,
            std::move(dependencies), 0, 0);
        material_resources_.emplace(material.value,
                                    TrackedMaterial{std::move(resource), std::move(payload)});
        return;
    }

    auto retired = coordinator_.registry().publish({watch::StagedResource{
        found->second.resource, payload, std::move(dependencies), 0, 0}});
    found->second.live_payload = std::move(payload);
    // Descriptor registry payloads are CPU-only markers.
    retired.clear();
}

void TextureReloadHandler::track(const watch::AssetKey &key, std::filesystem::path path,
                                 GlobalTextureId texture) {
    if (textures_.contains(key)) {
        throw std::runtime_error("reloadable texture is already tracked: " +
                                 watch::assetKeyString(key));
    }
    const auto &resource = materials_.textures.get(texture);
    auto payload = std::make_shared<TexturePayload>();
    payload->key = key;
    payload->texture = texture;
    payload->width = resource.image.extent.width;
    payload->height = resource.image.extent.height;
    payload->mip_levels = resource.image.mip_levels;
    payload->bytes = std::filesystem::file_size(path);
    auto logical = coordinator_.registry().declareResource(
        std::string{textureTable}, key, payload, {key}, 0, payload->bytes);
    textures_.emplace(key, TrackedTexture{std::move(path), texture, std::move(logical), payload});

    // Tracking may be enabled after a material was registered. Build the
    // reverse dependency entries from MaterialContainer's complete index.
    const auto reverse = materials_.texture_materials.find(texture);
    if (reverse != materials_.texture_materials.end()) {
        for (const auto material : reverse->second) refreshMaterial(material);
    }
}

void TextureReloadHandler::materialRegistered(GlobalMaterialId material) {
    refreshMaterial(material);
}

bool TextureReloadHandler::enqueue(const watch::ReloadRequest &request,
                                   watch::ReloadCoordinator &coordinator) {
    if (&coordinator != &coordinator_) {
        throw std::runtime_error("texture reload used a different coordinator");
    }
    const auto found = textures_.find(request.key);
    if (found == textures_.end()) return false;

    const auto tracked = found->second;
    auto pending = std::make_shared<PendingReload>();
    const auto snapshot = coordinator_.registry().snapshot();
    const auto current = snapshot.find(tracked.resource);
    if (!current) throw std::runtime_error("reloadable texture resource is stale");

    watch::ReloadTransactionGroup group{"texture " + watch::assetKeyString(request.key)};
    group.add(watch::ReloadActor{
        .name = "texture",
        .target = tracked.resource,
        .parse = [pending, request, path = tracked.path] {
            if (request.kind == watch::ReloadKind::removed) {
                throw std::runtime_error("texture file is missing: " + path.string());
            }
            pending->decoded = std::make_shared<LoadedImage>(loadImageFile(path));
        },
        .validate = [this, pending, path = tracked.path, texture = tracked.texture] {
            if (!pending->decoded || pending->decoded->pixels.empty() ||
                pending->decoded->levels.empty()) {
                throw std::runtime_error("texture candidate is empty: " + path.string());
            }
            materials_.validateTextureReload(texture, *pending->decoded);
        },
        .stage = [this, pending, tracked, current] {
            auto payload = std::make_shared<TexturePayload>();
            payload->key = current->source;
            payload->texture = tracked.texture;
            payload->width = pending->decoded->width;
            payload->height = pending->decoded->height;
            payload->format = pending->decoded->format;
            payload->mip_levels = pending->decoded->mipLevels();
            payload->bytes = pending->decoded->pixels.size();
            payload->in_place = materials_.textureShapeMatches(tracked.texture, *pending->decoded);
            if (payload->in_place) {
                // All parse/validate closures have completed before stage starts;
                // this is the sole potentially-failing stage for an in-place group.
                materials_.uploadTextureInPlace(tracked.texture, *pending->decoded);
            } else {
                payload->replacement.emplace(
                    materials_.createTextureResource(*pending->decoded, tracked.path.string()));
                payload->descriptors = materials_.stageTextureRebind(
                    tracked.texture, *payload->replacement);
            }
            return watch::StagedResourceData{
                payload, {current->source},
                current->compatibility_revision + (payload->in_place ? 0u : 1u),
                payload->bytes};
        },
    });

    for (const auto &dependent : snapshot.reverseDependents(request.key)) {
        if (dependent.table != materialDescriptorTable) continue;
        const auto descriptor = snapshot.find(dependent);
        if (!descriptor) throw std::runtime_error("material descriptor dependency is stale");
        const auto marker = descriptor->payloadAs<MaterialDescriptorPayload>();
        const auto dependencies = materialDependencies(marker->material);
        group.add(watch::ReloadActor{
            .name = "material descriptor " + std::to_string(marker->material.value),
            .target = dependent,
            .after = {tracked.resource},
            .stage = [marker, dependencies] {
                return watch::StagedResourceData{
                    std::make_shared<MaterialDescriptorPayload>(*marker), dependencies, 0, 0};
            },
        });
    }

    coordinator_.enqueue(std::move(group));
    return true;
}

bool TextureReloadHandler::retire(std::shared_ptr<const void> payload,
                                  watch::ReloadCoordinator &coordinator) noexcept {
    if (&coordinator != &coordinator_) return false;
    for (auto &[key, tracked] : textures_) {
        if (tracked.live_payload.get() != payload.get()) continue;
        try {
            const auto current = coordinator_.registry().snapshot().find(tracked.resource);
            if (!current) return false;
            auto next = current->payloadAs<TexturePayload>();
            if (next->replacement) {
                auto old = materials_.commitTextureRebind(
                    tracked.texture, std::move(*next->replacement),
                    std::move(next->descriptors));
                next->replacement.reset();
                GET_MODULE(DeletionQueue).defer(std::move(old.texture));
                for (auto &descriptor : old.descriptors) {
                    GET_MODULE(DeletionQueue).defer(std::move(descriptor));
                }
            }
            tracked.live_payload = current->payload;
            return true;
        } catch (const std::exception &error) {
            if (logger) LOG_ERROR(logger, "texture reload retirement failed: {}", error.what());
            return true;
        } catch (...) {
            if (logger) LOG_ERROR(logger, "texture reload retirement failed with unknown error");
            return true;
        }
    }
    return false;
}

} // namespace Pelican
