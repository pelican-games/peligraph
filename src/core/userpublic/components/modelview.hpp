#pragma once

#include "../../renderer/modelinstance.hpp"
#include "../details/schema/structfieldschema.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace Pelican {

// MT-2: one row of simplemodelview.materials, the scene object's own
// material for its model. With mesh and primitive it covers that GLB
// primitive; with neither, every primitive. The model's material
// (asset_data material_bindings, else the glTF material) is what a primitive
// no row covers draws with.
struct ModelMaterialOverride {
    std::string material;
    std::optional<std::uint32_t> mesh;
    std::optional<std::uint32_t> primitive;

    static constexpr auto schema = structFields(
        componentPolicy("model_material_override"),
        required(field<&ModelMaterialOverride::material>("material")),
        optionalOverride(field<&ModelMaterialOverride::mesh>("mesh", urange(0, 0xffffffffu))),
        optionalOverride(field<&ModelMaterialOverride::primitive>("primitive", urange(0, 0xffffffffu))));

    bool operator==(const ModelMaterialOverride &) const = default;
};

struct SimpleModelViewComponent {
    std::string model_name;
    uint8_t dirty = 0;
    std::optional<ModelInstanceId> model_instance_id;
    // Runtime-only join key. SceneLoader assigns the stable authoring identity
    // after entity creation; it is never serialized by ref().
    std::uint64_t authoring_object_id = 0;
    // MT-2: the authored simplemodelview.materials list (absent: none).
    std::optional<List<ModelMaterialOverride>> materials;

    template <class T> void ref(T &ar) { ar.prop("model", model_name); }

    void init();
    void deinit() noexcept;
};

// MT-2: the overrides a model instance is staged with, or null for none.
std::shared_ptr<const InstanceMaterialOverrides>
instanceMaterialOverrides(const std::optional<List<ModelMaterialOverride>> &materials);

} // namespace Pelican
