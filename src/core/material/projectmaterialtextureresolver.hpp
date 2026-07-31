#pragma once

#include "material.hpp"
#include "../watch/assetkey.hpp"

#include <map>
#include <string_view>

namespace Pelican {

class MaterialContainer;
class PathResolver;
class StandardMaterialResource;

// Long-lived resolver for project material texture references. Project files
// are registered through the reloadable texture path once per logical key;
// engine semantic textures retain the permanent StandardMaterialResource IDs.
class ProjectMaterialTextureResolver {
    MaterialContainer &materials_;
    StandardMaterialResource &standard_;
    PathResolver &paths_;
    std::map<watch::AssetKey, GlobalTextureId>
        project_textures_;

  public:
    ProjectMaterialTextureResolver(
        MaterialContainer &materials,
        StandardMaterialResource &standard,
        PathResolver &paths);

    GlobalTextureId resolve(
        std::string_view reference,
        SurfaceTextureRole role);
};

} // namespace Pelican
