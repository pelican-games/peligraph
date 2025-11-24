#include "model.hpp"
#include "../loader/basicconfig.hpp"
#include "../model/gltf.hpp"
#include <nlohmann/json.hpp>

namespace Pelican {

ModelAssetContainer::ModelAssetContainer() {
    auto &loader = GET_MODULE(GltfLoader);

    const auto model_assets = nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()).at("models");
    for (const auto &model_asset : model_assets) {
        model_templates.insert({
            model_asset.at("name"),
            loader.loadGltfBinary(model_asset.at("path")),
        });
    }
}

ModelTemplate &ModelAssetContainer::getModelTemplateByName(const std::string &name) { return model_templates.at(name); }

} // namespace Pelican
