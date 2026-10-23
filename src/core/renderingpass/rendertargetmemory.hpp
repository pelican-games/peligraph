#pragma once

#include "rendertargetmetadata.hpp"
#include "../../project/targetrendermemory.hpp"

#include <cstdint>
#include <optional>

namespace Pelican {

// RC-1b: a registered render target as get_frame_plan's memory total counts
// it: renderTargetBytes(metadata), of which its single-sample images are
// what the members of its alias group (`alias_group`, the registry's token)
// share; each member renders into a multisampled image of its own.
// (Defined with RenderTargetContainer, which makes the images.)
RegisteredTargetMemory registeredRenderTargetMemory(
    const RenderTargetMetadata &metadata,
    std::optional<std::uint64_t> alias_group, bool used);

} // namespace Pelican
