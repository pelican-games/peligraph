#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace Pelican {

// Runtime owner of material documents declared by pelican.asset_data v1.
// The exported span is the additive binding-resolution domain consumed by
// ModelAssetContainer; the GPU records themselves remain MaterialContainer
// resources.
DECLARE_MODULE(ProjectMaterialAssetContainer) {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ProjectMaterialAssetContainer();
    ~ProjectMaterialAssetContainer();
    ProjectMaterialAssetContainer(
        const ProjectMaterialAssetContainer &) = delete;
    ProjectMaterialAssetContainer &operator=(
        const ProjectMaterialAssetContainer &) = delete;

    std::span<const ModelTemplate::NamedMaterial>
    namedMaterials() const noexcept;
    GlobalMaterialId
    materialByName(std::string_view name) const;
};

} // namespace Pelican
