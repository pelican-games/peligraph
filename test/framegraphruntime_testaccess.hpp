#pragma once

#include "../src/core/renderingpass/framegraphruntime.hpp"

namespace Pelican::test_support {

struct FrameGraphRuntimeTestAccess {
    static std::uint64_t scopeMappingBuilds() noexcept {
        return FrameGraphScopeMapping::builds.load(std::memory_order_relaxed);
    }
};

} // namespace Pelican::test_support
