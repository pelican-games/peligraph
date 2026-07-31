#include "projectmaterialtextureresolver.hpp"

#include "../loader/pathresolver.hpp"
#include "materialcontainer.hpp"
#include "standardmaterialresource.hpp"

#include <stdexcept>
#include <string>

namespace Pelican {
namespace {

constexpr std::string_view engineTexturePrefix =
    "engine://textures/";

} // namespace

ProjectMaterialTextureResolver::
    ProjectMaterialTextureResolver(
        MaterialContainer &materials,
        StandardMaterialResource &standard,
        PathResolver &paths)
    : materials_{materials},
      standard_{standard},
      paths_{paths} {}

GlobalTextureId ProjectMaterialTextureResolver::resolve(
    std::string_view reference,
    SurfaceTextureRole role) {
    (void)role;
    if (reference.starts_with(engineTexturePrefix)) {
        const auto name =
            reference.substr(engineTexturePrefix.size());
        if (name == "white") {
            return standard_.whiteTexture();
        }
        if (name == "black") {
            return standard_.blackTexture();
        }
        if (name == "flat_normal") {
            return standard_.normalDefaultTexture();
        }
        if (name == "flat_gray") {
            return standard_.grayTexture();
        }
        throw std::runtime_error(
            "unknown engine material texture '" +
            std::string{reference} + "'");
    }
    if (reference.starts_with("engine://")) {
        throw std::runtime_error(
            "engine material texture must use "
            "engine://textures/: " +
            std::string{reference});
    }

    const auto parsed = parsePathReference(reference);
    if (parsed.fragment) {
        throw std::runtime_error(
            "project material texture must not contain "
            "a fragment: " +
            std::string{reference});
    }
    const auto key = watch::makeAssetKey(reference);
    if (const auto found =
            project_textures_.find(key);
        found != project_textures_.end()) {
        return found->second;
    }
    const auto path = paths_.resolveExistingFile(reference);
    const auto texture =
        materials_.registerReloadableTextureFile(key, path);
    project_textures_.emplace(key, texture);
    return texture;
}

} // namespace Pelican
