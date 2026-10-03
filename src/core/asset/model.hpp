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
#include <vector>

namespace Pelican::watch {
struct AssetKey;
struct ReloadRequest;
class ReloadCoordinator;
class ReloadTransactionGroup;
} // namespace Pelican::watch

namespace Pelican {

struct PrimitiveMaterialBindingDocument;

// MT-1: one primitive of a model as its file (or generated shape) declares it:
// its stable GLB coordinates and the glTF material it carries there (`slot`,
// empty when the primitive names none), with that material's routing.
struct ModelMaterialSlot {
    std::uint32_t mesh = 0;
    std::uint32_t primitive = 0;
    std::string slot;
    std::optional<MaterialVariantRouting> routing;
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

    ModelAssetId assetIdForTesting(const std::string &name) const;
    std::uint64_t contentRevisionForTesting(const std::string &name) const;
    std::uint64_t compatibilityRevisionForTesting(const std::string &name) const;
    nlohmann::json occurrenceCatalogStatus() const;
};

} // namespace Pelican
