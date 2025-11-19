#include "scene.hpp"

#include "../model/gltf.hpp"
#include "../renderer/camera.hpp"
#include "../renderer/polygoninstancecontainer.hpp"

namespace Pelican {

Scene::Scene() {}
Scene::~Scene() {}

void Scene::load() {
    // for test
    auto &camera = GET_MODULE(Camera);
    camera.setPos({3.0, 3.0, 3.0});
    camera.setDir({-3.0, -3.0, -3.0});

    auto &loader = GET_MODULE(GltfLoader);
    auto model = loader.loadGltfBinary("AliciaSolid.vrm");

    auto &pic = GET_MODULE(PolygonInstanceContainer);
    pic.placeModelInstance(model);
}

} // namespace Pelican