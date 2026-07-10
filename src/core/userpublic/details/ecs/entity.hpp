#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <limits>
#include <ostream>
#include <string>

namespace Pelican {

struct EntityId {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;

    auto operator<=>(const EntityId &) const = default;
};

using GameObjectId = EntityId;

inline constexpr EntityId invalidEntityId{};
inline constexpr GameObjectId invalidGameObjectId{};

inline std::string toString(EntityId id) {
    return std::to_string(id.index) + ":" + std::to_string(id.generation);
}

inline std::ostream &operator<<(std::ostream &stream, EntityId id) {
    return stream << id.index << ':' << id.generation;
}

}

template <> struct std::hash<Pelican::EntityId> {
    size_t operator()(Pelican::EntityId id) const noexcept {
        const auto packed = (static_cast<std::uint64_t>(id.index) << 32U) | id.generation;
        return std::hash<std::uint64_t>{}(packed);
    }
};
