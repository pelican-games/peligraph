#pragma once

#include "../container.hpp"
#include "../loader/pathresolver.hpp"
#include "modeltemplate.hpp"
#include "occurrenceidentity.hpp"
#include "modelresourcerelease.hpp"
#include "../../project/shaderresourceport.hpp"
#include <memory>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

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
    // Container and external buffers used by model/animation data. Images have
    // their own texture reload path and are not model-rebuild dependencies.
    std::vector<std::filesystem::path> dataFiles() const;
};

// Provenance-bearing logical reference supplied by the project/path resolver.
// A filesystem path is intentionally not implicitly convertible to this type.
struct GltfAuthoredAssetReference {
    std::string value;
};

struct GltfAttributeData {
    ShaderResourceBufferElement element;
    std::uint64_t count;
    std::vector<std::byte> bytes;
    std::vector<std::filesystem::path> source_files;
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
    GltfAttributeData readAccessor(const PreparedGltf &prepared, int accessor,
        ShaderResourceBufferElement element) const;

  public:
    GltfLoader();
    PreparedGltf prepareGltfBinary(std::string path,
                                   std::optional<AssetFragmentRef> fragment = std::nullopt,
                                   std::optional<CanonicalModelAssetKey> asset_key = std::nullopt) const;
    PreparedGltf prepareGltfBinarySceneNode(std::string path,
                                            AssetFragmentRef fragment,
                                            std::optional<CanonicalModelAssetKey> asset_key = std::nullopt) const;
    PreparedGltf prepareGltf(std::string path,
                             std::optional<AssetFragmentRef> fragment = std::nullopt,
                             std::optional<CanonicalModelAssetKey> asset_key = std::nullopt) const;
    // GP-5: an engine://primitives/<name> shape, generated in memory as a
    // glTF document (engineprimitives.hpp). No file backs it, so dataFiles()
    // is empty. Unknown names throw model.primitive_unknown@1.
    PreparedGltf prepareEnginePrimitive(std::string_view reference,
                                        std::optional<CanonicalModelAssetKey> asset_key = std::nullopt) const;
    // Runs fragment selection, accessor extraction, rig construction, and
    // primitive/material validation without allocating or uploading GPU
    // resources. The returned template contains candidate topology/rig data
    // with synthetic draw handles and is suitable for reload compatibility
    // checks only.
    ModelTemplate inspect(const PreparedGltf &prepared) const;
    void validateMeshNode(const PreparedGltf &prepared, std::uint32_t node, std::uint32_t mesh) const;
    // Read source mesh coordinates, before node transforms, morphing or skinning.
    // Uses the prepared source document; no geometry/material GPU allocations.
    GltfAttributeData readAttribute(const PreparedGltf &prepared,
        std::uint32_t mesh, std::uint32_t primitive, std::string_view semantic,
        ShaderResourceBufferElement element,
        std::optional<std::uint32_t> morph_target = std::nullopt) const;
    // Indexed primitives only. Converts glTF unsigned index widths to uint32.
    GltfAttributeData readIndices(const PreparedGltf &prepared,
        std::uint32_t mesh, std::uint32_t primitive) const;
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
[[nodiscard]] ReleaseReport releaseModelGpuResources(ModelTemplate &model,
                                                     bool deferred) noexcept;

} // namespace Pelican
