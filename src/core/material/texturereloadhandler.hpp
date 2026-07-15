#pragma once

#include "../watch/reloadqueue.hpp"
#include "../watch/reloadtransaction.hpp"
#include "material.hpp"

#include <filesystem>
#include <map>
#include <memory>

namespace Pelican {

class MaterialContainer;

// HR1-T adapter. It only supplies resource-specific parse/validate/stage
// closures; identity, rollback, reverse edges, publication, and status remain
// owned by ReloadCoordinator.
class TextureReloadHandler {
  public:
    TextureReloadHandler(MaterialContainer &materials, watch::ReloadCoordinator &coordinator);
    ~TextureReloadHandler();

    void track(const watch::AssetKey &key, std::filesystem::path path,
               GlobalTextureId texture);
    void materialRegistered(GlobalMaterialId material);
    bool handles(const watch::AssetKey &key) const;
    bool enqueue(const watch::ReloadRequest &request,
                 watch::ReloadCoordinator &coordinator);
    bool retire(std::shared_ptr<const void> payload,
                watch::ReloadCoordinator &coordinator) noexcept;

  private:
    struct TexturePayload;
    struct MaterialDescriptorPayload;
    struct TrackedTexture;
    struct TrackedMaterial;
    struct PendingReload;

    void refreshMaterial(GlobalMaterialId material);
    std::vector<watch::AssetKey> materialDependencies(GlobalMaterialId material) const;

    MaterialContainer &materials_;
    watch::ReloadCoordinator &coordinator_;
    std::map<watch::AssetKey, TrackedTexture> textures_;
    std::map<int, TrackedMaterial> material_resources_;
};

} // namespace Pelican
