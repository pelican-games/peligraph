#include "model.hpp"
#include "../model/gltf.hpp"

namespace Pelican {

ModelAssetContainer::ModelAssetContainer() {
    // TODO: load from file
    auto &loader = GET_MODULE(GltfLoader);
    model_templates.insert({"alicia", loader.loadGltfBinary("AliciaSolid.vrm")});
}

ModelTemplate &ModelAssetContainer::getModelTemplateByName(const std::string &name) { return model_templates.at(name); }

} // namespace Pelican
