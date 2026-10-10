#pragma once
#include "../loader/imageloader.hpp"
#include "atlasasset.hpp"
#include "../loader/pathresolver.hpp"
#include "../userpublic/userdataerror.hpp"
#include <fstream>
#include <nlohmann/json.hpp>
namespace Pelican {
inline asset::ResolvedSpriteAsset requireSpriteTexture(
    const asset::SpriteAssetCatalog &catalog, std::string_view reference,
    const PathResolver &resolver) {
    try {
        auto result = catalog.resolve(reference,
            [&](std::string_view path) { return resolver.resolveExistingFile(std::string{path}); },
            [](const std::filesystem::path &path) {
                std::ifstream input{path};
                if (!input) throw UserDataError("failed to open sprite asset: " + path.string());
                auto atlas = asset::parseAtlasAssetV1(nlohmann::json::parse(input), path);
                for (const auto &page : atlas.pages)
                    if (!std::filesystem::is_regular_file(page.image_path))
                        throw UserDataError("sprite atlas page not found: " + page.image_path.string());
                for (const auto &page : atlas.pages) (void)loadImageFile(page.image_path);
                return atlas;
            });
        if (!asset::parseSpriteAssetReference(reference).atlas_fragment)
            (void)loadImageFile(result.source_path);
        return result;
    } catch (const std::runtime_error &error) { throw UserDataError(error.what()); }
      catch (const nlohmann::json::exception &error) { throw UserDataError(error.what()); }
}
}
