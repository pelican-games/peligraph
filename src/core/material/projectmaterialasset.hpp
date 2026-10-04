#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"

#include <memory>
#include <optional>
#include <string>
#include <span>
#include <string_view>
#include <vector>

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

    // MT-1 (create_material): reads one material document the project's
    // asset_data now lists and registers its materials, as the constructor
    // registers every listed document at start (values reload included).
    // Throws, registering nothing, when the document does not parse, names a
    // surface that is missing, or holds a name already registered. Returns
    // the names registered.
    std::vector<std::string> registerDocument(
        const std::string &reference);
    // MT-3 (edit_material): reads a registered document again and registers
    // its materials anew -- shader, pipeline, textures, tags, route and
    // variants as the document now says -- under the same names; its values
    // are reloaded from the new registration on. The previous registrations
    // are returned still registered: what draws with them (model templates,
    // draw items) holds their ids until it is rebuilt, and the caller
    // releases them after that. Throws, changing nothing, as
    // registerDocument does.
    struct Reregistered {
        std::vector<std::string> names;
        std::vector<GlobalMaterialId> previous;
    };
    Reregistered reregisterDocument(const std::string &reference);
    // EB-1 (delete_material): takes the document's materials off the names
    // (and its values off the material values reload) and returns their
    // registrations, still registered: the caller releases them once nothing
    // draws with them. Nothing when the document registered nothing here.
    std::vector<GlobalMaterialId> unregisterDocument(
        const std::string &reference);
    // MT-1: the document a registered material was read from (its asset_data
    // materials[] reference), or nullopt for a name that is not registered.
    std::optional<std::string> documentOf(
        std::string_view name) const;
};

} // namespace Pelican
