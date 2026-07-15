#pragma once

#include "../watch/reloadqueue.hpp"
#include "../watch/reloadtransaction.hpp"
#include "materialcontainer.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>

namespace Pelican {

// Material values keep their logical IDs while HR2-S may replace the real
// surface layout and every dependent value payload in one shader transaction.
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
    std::function<void()> prepareSurfaceReload(
        const std::map<watch::AssetKey, SurfaceFormatDocument> &surface_documents,
        std::span<const watch::AssetKey> material_documents);

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
