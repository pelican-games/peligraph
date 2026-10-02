#pragma once

#include <cstdint>
#include <string>

namespace Pelican {

// EL-1 / EL-6: one provider a document may name in a render provider
// registry (draw sort, pass implementation, subgraph replacement, graph
// transform, render strategy), as the registry would resolve that name now:
// a game provider that shadows the engine's under one name is listed, the
// shadowed engine one is not. The registries' ABIs carry no description and
// no parameter declaration, so a listing has neither.
struct RenderProviderListing {
    std::string name;
    // Registered by the engine itself (a builtin provider), not by the game
    // DLL.
    bool engine_owned = false;
    std::uint32_t provider_version = 0;

    bool operator==(const RenderProviderListing &) const = default;
};

} // namespace Pelican
