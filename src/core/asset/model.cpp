#include "model.hpp"
#include "../loader/basicconfig.hpp"
#include "../model/gltf.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace Pelican {

ModelAssetContainer::ModelAssetContainer() {
    auto &loader = GET_MODULE(GltfLoader);

    const auto model_assets = nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()).at("models");
    for (const auto &model_asset : model_assets) {
        const std::string model_path_string = model_asset.at("path");
        const auto model_path = std::filesystem::path{model_path_string};
        auto extension = model_path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        model_templates.insert({
            model_asset.at("name"),
            extension == ".gltf" ? loader.loadGltf(model_path_string) : loader.loadGltfBinary(model_path_string),
        });
    }
}

ModelTemplate &ModelAssetContainer::getModelTemplateByName(const std::string &name) { return model_templates.at(name); }

} // namespace Pelican
