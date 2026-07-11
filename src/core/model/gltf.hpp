#pragma once

#include "../container.hpp"
#include "../loader/pathresolver.hpp"
#include "modeltemplate.hpp"
#include <memory>

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

DECLARE_MODULE(GltfLoader) {
    

  public:
    GltfLoader();
    PreparedGltf prepareGltfBinary(std::string path,
                                   std::optional<AssetFragmentRef> fragment = std::nullopt) const;
    PreparedGltf prepareGltf(std::string path,
                             std::optional<AssetFragmentRef> fragment = std::nullopt) const;
    ModelTemplate commit(PreparedGltf prepared) const;
    ModelTemplate loadGltfBinary(std::string path,
                                 std::optional<AssetFragmentRef> fragment = std::nullopt);
    ModelTemplate loadGltfBinarySceneNode(std::string path, AssetFragmentRef fragment);
    ModelTemplate loadGltf(std::string path,
                           std::optional<AssetFragmentRef> fragment = std::nullopt);
};

} // namespace Pelican
