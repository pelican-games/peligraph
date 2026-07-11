#pragma once

#include "../container.hpp"
#include "../loader/pathresolver.hpp"
#include "modeltemplate.hpp"

namespace Pelican {

DECLARE_MODULE(GltfLoader) {
    

  public:
    GltfLoader();
    ModelTemplate loadGltfBinary(std::string path,
                                 std::optional<AssetFragmentRef> fragment = std::nullopt);
    ModelTemplate loadGltf(std::string path,
                           std::optional<AssetFragmentRef> fragment = std::nullopt);
};

} // namespace Pelican
