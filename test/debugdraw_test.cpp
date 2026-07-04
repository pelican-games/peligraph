#include "../src/core/renderer/debugdraw.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("DebugDraw CPU API is a no-op before the feature registers a pass", "[debug-draw]") {
    DebugDraw debug_draw;

    debug_draw.line({-0.5f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f});

    REQUIRE_FALSE(debug_draw.isEnabledForTesting());
    REQUIRE(debug_draw.queuedVertexCountForTesting() == 0);
}

} // namespace Pelican
