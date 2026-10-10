#pragma once

#include "../container.hpp"
#include "../model/modeltemplate.hpp"

#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <stdexcept>
#include <vector>

namespace Pelican::watch {
struct AssetKey;
struct ReloadRequest;
class ReloadCoordinator;
class ReloadTransactionGroup;
} // namespace Pelican::watch

namespace Pelican {

struct PrimitiveMaterialBindingDocument;

// MI-1: getModelTemplateByName of a name the container does not hold (and
// that is no engine primitive or fragment reference). An edit that fails on
// it names the model to the editor.
class UnknownModelAssetError final : public std::out_of_range {
    std::string model_;

  public:
    explicit UnknownModelAssetError(std::string model)
        : std::out_of_range{"unknown model asset: " + model}, model_{std::move(model)} {}
    const std::string &model() const noexcept { return model_; }
};

// MT-1: one primitive of a model as its file (or generated shape) declares it:
// its stable GLB coordinates and the glTF material it carries there (`slot`,
// empty when the primitive names none), with that material's routing.
struct ModelMaterialSlot {
    std::uint32_t mesh = 0;
    std::uint32_t primitive = 0;
    std::string slot;
    std::optional<MaterialVariantRouting> routing;
    bool vat_deformed = false;
};

// Logical owner of project model templates. Physical glTF files and fragment
// addresses stay implementation details; callers retain a stable ModelAssetId
// across content revisions.
DECLARE_MODULE(ModelAssetContainer) {
    struct Impl;
    std::unique_ptr<Impl> impl_;

  public:
    ModelAssetContainer();
    ~ModelAssetContainer();
    ModelAssetContainer(const ModelAssetContainer &) = delete;
    ModelAssetContainer &operator=(const ModelAssetContainer &) = delete;

    ModelTemplate &getModelTemplateByName(const std::string &name);

    void attachReloadCoordinator(watch::ReloadCoordinator &coordinator);
    bool handlesReload(const watch::AssetKey &key) const;
    bool enqueueReload(const watch::ReloadRequest &request,
                       watch::ReloadCoordinator &coordinator);
    watch::ReloadTransactionGroup makeReloadGroup(std::span<const watch::ReloadRequest> requests,
                                                 watch::ReloadCoordinator &coordinator);
    bool retireReloadPayload(std::shared_ptr<const void> payload,
                             watch::ReloadCoordinator &coordinator) noexcept;

    // MT-1 (bind_material). Whether a template of that name is held now.
    bool hasModel(const std::string &name) const;
    // Every primitive of the model with the glTF material it carries in the
    // file, whatever the bindings say, ordered by mesh then primitive. Reads
    // the model's file (or regenerates the shape) again; nothing is
    // registered.
    std::vector<ModelMaterialSlot> materialSlots(const std::string &name) const;
    // The material bindings document the model's declaration carries now
    // (nullopt: none).
    std::optional<PrimitiveMaterialBindingDocument>
    declaredMaterialBindings(const std::string &name) const;
    // Reads `reference` as the model's material bindings document, checked
    // against the model as at start, and puts it in the declaration in place
    // of what it carried (returned). The template keeps the previous bindings
    // until a reload of reloadKey(name) rebuilds it.
    std::optional<PrimitiveMaterialBindingDocument>
    exchangeMaterialBindings(const std::string &name, const std::string &reference);
    // Puts `previous` back (a reload that failed after the exchange).
    void restoreMaterialBindings(const std::string &name,
                                 std::optional<PrimitiveMaterialBindingDocument> previous);
    // The source a reload of this model is requested by.
    watch::AssetKey reloadKey(const std::string &name) const;
    // MT-1 (create_material): a model held now whose file declares a glTF
    // material named `material` (a project material of that name would make
    // the model's bindings ambiguous and stop it loading), or nullopt.
    std::optional<std::string> modelDeclaringMaterial(std::string_view material) const;
    // MT-3 (edit_material): the models held now that draw with one of
    // `materials` -- their template's primitives (bindings included), or a
    // live instance's own material (MT-2) -- by name.
    std::vector<std::string> modelsDrawingMaterials(std::span<const GlobalMaterialId> materials) const;

    // MI-1 (register_model): whether the project file `reference` loads as a
    // model named `name` the way a models[] entry loads at start -- resolved,
    // read, inspected (no GPU allocation) and its glTF materials checked
    // against the project's. Registers nothing; throws with the reason.
    static void preflightModel(const std::string &name, const std::string &reference);
    // MI-1: registers the models[] entry `name` of the asset_data the engine
    // has published (ProjectBasicConfig::assetDataJson), as the constructor
    // registers each entry at start: the same declaration, reload sources and
    // logical resource. For a record retained from the same file, adopts the
    // new declaration and rebuilds through the model reload transaction.
    // Throws if the entry is absent, the name belongs to another file, or
    // loading fails. A retained record keeps the new declaration on failure
    // and is marked for replacement at restart; its live template is kept.
    void registerModel(const std::string &name);
    // A retained registration whose declaration has not reached the live
    // template. Cleared by successful model reload publication.
    bool registrationRestartRequired(const std::string &name) const;
    // MI-1 (unregister_model): releases the template `name` and its reload
    // declaration. nullopt: released, or not held. Otherwise the reason it is
    // kept until the engine starts again (a live instance still draws it, or
    // its animation rig may still be referenced); nothing is changed then.
    std::optional<std::string> unregisterModel(const std::string &name);

    ModelAssetId assetIdForTesting(const std::string &name) const;
    std::uint64_t contentRevisionForTesting(const std::string &name) const;
    std::uint64_t compatibilityRevisionForTesting(const std::string &name) const;
    nlohmann::json occurrenceCatalogStatus() const;
};

} // namespace Pelican
