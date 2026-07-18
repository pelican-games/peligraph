#include "golden_harness.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

TEST_CASE("Renderer execution matches plan order and captured traces",
          "[golden][headless][framegraph]") {
    GoldenHarness::runRendererTrace();
}

TEST_CASE("fullscreen inputs rebind after shader reload and render-target recreation",
          "[golden][headless][framegraph][rebind]") {
    GoldenHarness::runFullscreenRebind();
}

} // namespace Pelican
