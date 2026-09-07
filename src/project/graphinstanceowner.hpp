#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace Pelican {
// Persisted authoring identity, never a runtime EntityId.
struct GraphInstanceOwner {
    std::string scene;
    std::uint64_t object = 0;
};
using GraphInstanceOwners = std::map<std::string, GraphInstanceOwner, std::less<>>;
}
