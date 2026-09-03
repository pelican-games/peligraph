#pragma once

#include "../container.hpp"
#include "../loader/pathresolver.hpp"
#include "modeltemplate.hpp"
#include "modelresourcerelease.hpp"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace Pelican {

class PreparedGltf {
  public:
    struct Impl;

  private:
    std::shared_ptr<Impl> impl;

    explicit PreparedGltf(std::shared_ptr<Impl> value) : impl{std::move(value)} {}
    friend class GltfLoader;

  public:
    PreparedGltf() = default;
};

// Provenance-bearing logical reference supplied by the project/path resolver.
// A filesystem path is intentionally not implicitly convertible to this type.
struct GltfAuthoredAssetReference {
    std::string value;
};

class ModelResourceReleaseException : public std::runtime_error {
    ReleaseReport release_report_;

  public:
    ModelResourceReleaseException(std::string message,
                                  ReleaseReport release_report)
        : std::runtime_error{std::move(message)},
          release_report_{release_report} {}

    const ReleaseReport &releaseReport() const noexcept {
        return release_report_;
    }
};

DECLARE_MODULE(GltfLoader) {
    

  public:
    GltfLoader();
    PreparedGltf prepareGltfBinary(std::string path,
                                   std::optional<AssetFragmentRef> fragment = std::nullopt) const;
    PreparedGltf prepareGltfBinarySceneNode(std::string path,
                                            AssetFragmentRef fragment) const;
    PreparedGltf prepareGltf(std::string path,
                             std::optional<AssetFragmentRef> fragment = std::nullopt) const;
    // Runs fragment selection, accessor extraction, rig construction, and
    // primitive/material validation without allocating or uploading GPU
    // resources. The returned template contains candidate topology/rig data
    // with synthetic draw handles and is suitable for reload compatibility
    // checks only.
    ModelTemplate inspect(const PreparedGltf &prepared) const;
    ModelTemplate commit(
        PreparedGltf prepared,
        std::optional<GltfAuthoredAssetReference> authored_reference =
            std::nullopt) const;
    ModelTemplate loadGltfBinary(std::string path,
                                 std::optional<AssetFragmentRef> fragment = std::nullopt);
    ModelTemplate loadGltfBinarySceneNode(std::string path, AssetFragmentRef fragment);
    ModelTemplate loadGltf(std::string path,
                           std::optional<AssetFragmentRef> fragment = std::nullopt);
};

// Returns a failed candidate immediately. When deferred is true, material
// slots, Vulkan objects, and geometry ranges remain unavailable/alive for the
// configured in-flight window as required by their command-buffer lifetime.
ReleaseReport releaseModelGpuResources(ModelTemplate &model,
                                       bool deferred) noexcept;

} // namespace Pelican
