#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION

#include "gltf.hpp"
#include "primitivebufcontainer.hpp"
#include "vertbufcontainer.hpp"
#include <tiny_gltf.h>

namespace Pelican {

GltfLoader::GltfLoader(DependencyContainer &_con) : con{_con} {}

void GltfLoader::loadGltf(std::string path) {
    const auto &vertbuf_container = con.get<VertBufContainer>();
    // TODO
}

void GltfLoader::placeModel(/* TODO */) {
    const auto &primbuf_container = con.get<PrimitiveBufContainer>();
    // TODO
}

} // namespace Pelican
