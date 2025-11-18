#pragma once

#include "../container.hpp"
#include "modeltemplate.hpp"

namespace Pelican {

DECLARE_MODULE(GltfLoader) {
    

  public:
    GltfLoader();
    ModelTemplate loadGltfBinary(std::string path);
    ModelTemplate loadGltf(std::string path);
};

} // namespace Pelican
