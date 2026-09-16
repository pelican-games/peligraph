#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace Pelican {
// Persisted authoring identity, never a runtime EntityId.
struct GraphInstanceOwner {
    std::string scene;
    std::uint64_t object = 0;
    bool operator==(const GraphInstanceOwner &) const = default;
};
using GraphInstanceOwners = std::map<std::string, GraphInstanceOwner, std::less<>>;

// The owner of an expanded node, parameter or input name: the innermost
// instance scope its `/`-separated prefix names. One spelling, so a backend
// that resolves owners agrees with the scalar graph by construction.
inline const GraphInstanceOwner *graphInstanceOwnerOf(
    const GraphInstanceOwners &owners, std::string_view name) noexcept {
    auto slash = name.rfind('/');
    while (slash != std::string_view::npos) {
        const auto found = owners.find(name.substr(0, slash));
        if (found != owners.end()) return &found->second;
        if (slash == 0) break;
        slash = name.rfind('/', slash - 1);
    }
    return nullptr;
}
}
