#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Pelican::watch {
struct AssetKey;
struct ReloadRequest;
class ReloadCoordinator;
} // namespace Pelican::watch

namespace Pelican {

// Logical owner of project model templates. Physical glTF files and fragment
// addresses stay implementation details; callers retain a stable ModelAssetId
// across content revisions.
DECLARE_MODULE(ModelAssetContainer) {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ModelAssetContainer();
    ~ModelAssetContainer();
    ModelAssetContainer(const ModelAssetContainer &) = delete;
    ModelAssetContainer &operator=(const ModelAssetContainer &) = delete;

    ModelTemplate &getModelTemplateByName(const std::string &name);

    void attachReloadCoordinator(watch::ReloadCoordinator &coordinator);
    bool handlesReload(const watch::AssetKey &key) const;
    bool enqueueReload(const watch::ReloadRequest &request,
                       watch::ReloadCoordinator &coordinator);
    bool retireReloadPayload(std::shared_ptr<const void> payload,
                             watch::ReloadCoordinator &coordinator) noexcept;

    ModelAssetId assetIdForTesting(const std::string &name) const;
    std::uint64_t contentRevisionForTesting(const std::string &name) const;
    std::uint64_t compatibilityRevisionForTesting(const std::string &name) const;
};

} // namespace Pelican
