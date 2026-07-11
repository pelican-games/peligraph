#include "model.hpp"
#include "../loader/basicconfig.hpp"
#include "../loader/pathresolver.hpp"
#include "../model/gltf.hpp"
#include "../parallel_prepare.hpp"
#include "../startup.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <variant>

namespace Pelican {

namespace {
struct ModelDeclaration {
    std::string name;
    std::string path;
    std::optional<AssetFragmentRef> fragment;
    bool ascii = false;
};
} // namespace

ModelAssetContainer::ModelAssetContainer() {
    StartupPhaseTimer startup_timer{&StartupMetrics::addModels};

    const auto model_assets = nlohmann::json::parse(GET_MODULE(ProjectBasicConfig).assetDataJson()).at("models");
    std::vector<ModelDeclaration> declarations;
    declarations.reserve(model_assets.size());
    for (const auto &model_asset : model_assets) {
        const auto model_reference = model_asset.at("path").get<std::string>();
        const auto parsed_reference = parsePathReference(model_reference);
        const auto model_path = std::filesystem::path{parsed_reference.path};
        auto extension = model_path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        declarations.push_back(ModelDeclaration{
            .name = model_asset.at("name").get<std::string>(),
            .path = model_path.string(),
            .fragment = parsed_reference.fragment,
            .ascii = extension == ".gltf",
        });
    }

    // Parsing, buffer extraction, and stb image decode happen on workers. The
    // commit loop stays on the main thread and in JSON declaration order;
    // Vulkan uploads and resource/ECS-visible IDs therefore remain serial and
    // deterministic even when a later model finishes preparing first.
    const auto prepared = parallelPrepareOrdered<PreparedGltf>(declarations.size(), [&](std::size_t index) {
        GltfLoader loader;
        const auto &declaration = declarations[index];
        return declaration.ascii
                   ? loader.prepareGltf(declaration.path, declaration.fragment)
                   : loader.prepareGltfBinary(declaration.path, declaration.fragment);
    }, 4);

    auto &loader = GET_MODULE(GltfLoader);
    for (std::size_t index = 0; index < declarations.size(); ++index) {
        model_templates.emplace(declarations[index].name, loader.commit(prepared[index]));
    }
}

ModelTemplate &ModelAssetContainer::getModelTemplateByName(const std::string &name) {
    if (const auto found = model_templates.find(name); found != model_templates.end()) {
        return found->second;
    }

    const auto parsed = parsePathReference(name);
    if (!parsed.fragment) {
        return model_templates.at(name);
    }
    const auto resolved = GET_MODULE(PathResolver).resolveExistingFileReference(name);
    const auto *fragment = std::get_if<ResolvedPathFragment>(&resolved);
    if (fragment == nullptr) {
        throw std::runtime_error("model fragment reference did not resolve as a fragment: " + name);
    }
    auto extension = fragment->path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    auto &loader = GET_MODULE(GltfLoader);
    auto loaded = extension == ".gltf"
                      ? loader.loadGltf(fragment->path.string(), fragment->fragment)
                      : loader.loadGltfBinarySceneNode(fragment->path.string(), fragment->fragment);
    return model_templates.emplace(name, std::move(loaded)).first->second;
}

} // namespace Pelican
