#pragma once

#include <cstdint>
#include <string>

namespace Pelican {

// RC-7e: a declared render target the generation does not register (no image
// is made for it). unallocatedRenderTargets (targetrendermemory.hpp) names them.
struct UnallocatedRenderTarget {
    std::string name;
    // unused_in_generation: no graph of any runtime variant uses it.
    std::string reason;
    // What its images would occupy, from the declaration at the generation's
    // extent when it was registered (imageResourceBytes). An estimate fixed
    // at registration: a later container-only resize does not change it.
    std::uint64_t payload_bytes = 0;

    bool operator==(const UnallocatedRenderTarget &) const = default;
};

} // namespace Pelican
