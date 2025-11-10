#pragma once

#include "../container.hpp"

namespace Pelican {

class GltfLoader {
    DependencyContainer &con;

  public:
    GltfLoader(DependencyContainer &con);
    void loadGltf(std::string path);
    void placeModel(/* TODO */);
};

} // namespace Pelican
