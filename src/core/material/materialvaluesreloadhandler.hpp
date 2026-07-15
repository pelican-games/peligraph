#pragma once

#include "../watch/reloadqueue.hpp"
#include "../watch/reloadtransaction.hpp"
#include "materialcontainer.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <span>

namespace Pelican {

// HR1-M adapter. Surface resources are represented by compatibility-only fake
// actors until HR2-S owns real .surface parsing and cross-file transactions.
class MaterialValuesReloadHandler {
  public:
    MaterialValuesReloadHandler(MaterialContainer &materials,
                                watch::ReloadCoordinator &coordinator);
    ~MaterialValuesReloadHandler();

    void track(
        const watch::AssetKey &key, std::filesystem::path path,
        MaterialSurfaceCatalog surfaces,
        std::span<const MaterialContainer::ReloadableMaterialValuesBinding> bindings);
    bool handles(const watch::AssetKey &key) const;
    bool enqueue(const watch::ReloadRequest &request,
                 watch::ReloadCoordinator &coordinator);
    bool retire(std::shared_ptr<const void> payload,
                watch::ReloadCoordinator &coordinator) noexcept;

  private:
    struct DocumentPayload;
    struct SurfacePayload;
    struct ValuesPayload;
    struct PendingReload;
    struct TrackedSurface;
    struct TrackedMaterial;
    struct TrackedFile;

    MaterialContainer &materials_;
    watch::ReloadCoordinator &coordinator_;
    std::map<watch::AssetKey, TrackedFile> files_;
    std::map<watch::AssetKey, TrackedSurface> surfaces_;
};

} // namespace Pelican
