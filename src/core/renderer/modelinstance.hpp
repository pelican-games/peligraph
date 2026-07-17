#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>

namespace Pelican {

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
