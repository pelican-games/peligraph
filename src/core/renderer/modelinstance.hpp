#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace Pelican {

// MT-2: one material override a scene object puts on its model instance, by
// material name. `primitive` is the GLB {mesh, primitive} it covers; nullopt
// covers every primitive the instance draws. A primitive's own override wins
// over the every-primitive one, which wins over the model's material
// (asset_data material_bindings, else the glTF material).
struct InstanceMaterialOverride {
    std::optional<std::pair<std::uint32_t, std::uint32_t>> primitive;
    std::string material;

    bool operator==(const InstanceMaterialOverride &) const = default;
};

using InstanceMaterialOverrides = std::vector<InstanceMaterialOverride>;

struct ModelInstanceId {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    std::uint64_t scene_epoch = 0;

    auto operator<=>(const ModelInstanceId &) const = default;
};

inline constexpr ModelInstanceId invalidModelInstanceId{};

inline std::string toString(ModelInstanceId id) {
    return std::to_string(id.index) + ":" + std::to_string(id.generation) +
           "@" + std::to_string(id.scene_epoch);
}

inline std::ostream &operator<<(std::ostream &stream, ModelInstanceId id) {
    return stream << id.index << ':' << id.generation << '@' << id.scene_epoch;
}

} // namespace Pelican
