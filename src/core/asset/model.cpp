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
        const auto model_reference = model_asset.at("path").get<std::string>();
        const auto parsed_reference = parsePathReference(model_reference);
        const auto model_path = std::filesystem::path{parsed_reference.path};
        const auto model_path_string = model_path.string();
        auto extension = model_path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        model_templates.insert({
            model_asset.at("name"),
            extension == ".gltf"
                ? loader.loadGltf(model_path_string, parsed_reference.fragment)
                : loader.loadGltfBinary(model_path_string, parsed_reference.fragment),
        });
    }
}

ModelTemplate &ModelAssetContainer::getModelTemplateByName(const std::string &name) { return model_templates.at(name); }

} // namespace Pelican
