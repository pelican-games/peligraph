#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"
#include "../watch/assetkey.hpp"

#include <cstdint>
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

    // FO-1: a listed document the engine could not register at start. Without
    // --strict-assets the engine starts without its materials (the others are
    // registered as before) and lists it in the problem list:
    //   {severity: "error", source: "startup", code, message,
    //    document: "project://<path>", document_kind: "material",
    //    line?, column?, fields: {path, reason, line?, column?}}
    // code material.document_unknown@1 when the file is not there (status
    // "missing"), material.document_invalid@1 when it is not JSON (line and
    // column of the error), not a pelican.material document, or does not
    // register (a field the schema refuses, a surface that does not load, a
    // name another document holds; the reason says which) (status "invalid").
    // With --strict-assets the start fails as before.
    struct RefusedDocument {
        std::string status;
        std::string code;
        std::string reason;
        std::optional<std::uint64_t> line;
        std::optional<std::uint64_t> column;
    };
    std::optional<RefusedDocument> refusedDocument(
        std::string_view reference) const;
    bool hasRefusedDocuments() const noexcept;
    // FO-1: a model whose material bindings name materials no registered
    // document holds while a listed document was refused: its primitives keep
    // the model's own material, and the problem list has one warning for it:
    //   {severity: "warning", source: "startup", code:
    //    "material.binding_unresolved@1", message, document_kind: "material",
    //    fields: {model, materials: [...]}}
    // FO-1 / EB-2: `materials` empty takes the model's warning off (its
    // bindings resolve again).
    void noteUnresolvedBindings(const std::string &model,
                                std::vector<std::string> materials);

    // EB-2: a document refused at start, read again and registered now that
    // it reads (register_material {path} on a listed document, or the file
    // watch when `hot_reload` covers it). Throws, changing nothing (the
    // document stays refused and listed), when it still does not register.
    // Otherwise its problem leaves the list, and the models whose bindings
    // kept their own material for want of its materials are rebuilt in place
    // (ReloadService::reloadModelsNow), their warnings leaving with it;
    // `restart_reason` says why they could not be (the registration stays).
    struct Recovered {
        std::vector<std::string> names;
        std::vector<std::string> rebuilt_models;
        std::optional<std::string> restart_reason;
    };
    Recovered recoverDocument(const std::string &reference);
    // EB-2: the references of the documents refused at start.
    std::vector<std::string> refusedReferences() const;
    // EB-2: the refused document whose file the watch key names.
    std::optional<std::string> refusedReferenceOf(
        const watch::AssetKey &key) const;

    // EB-2: a registered document whose file was rewritten after start into
    // one that does not read (the values reload refused it; the engine keeps
    // what it read before) is listed as
    //   {severity: "error", source: "hot_reload", code:
    //    "material.document_invalid@1" | "material.document_unknown@1",
    //    message, document, document_kind: "material", line?, column?,
    //    fields: {path, reason, line?, column?}}
    // until a reload of it commits. A key that is no registered document of
    // this container is passed over.
    void noteReloadFailure(const watch::AssetKey &key,
                           const std::string &error);
    void clearReloadFailure(const watch::AssetKey &key) noexcept;
};

} // namespace Pelican
