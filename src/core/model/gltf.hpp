#pragma once

#include "../container.hpp"
#include "modeltemplate.hpp"

namespace Pelican {

class GltfLoader {
    DependencyContainer &con;

  public:
    GltfLoader(DependencyContainer &con);
    ModelTemplate loadGltf(std::string path);
};

} // namespace Pelican
