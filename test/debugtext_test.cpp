#include "../src/core/container.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

#include <catch2/catch_test_macros.hpp>

namespace Pelican {

namespace {

void ensureLogger() {
    static const bool initialized = [] {
        setupLogger();
        return true;
    }();
    (void)initialized;
}

} // namespace

TEST_CASE("DebugText CPU API is a no-op before the feature registers a pass", "[debug-text]") {
    DebugText debug_text;

    debug_text.text(0, 0, "WP54");

    REQUIRE_FALSE(debug_text.isEnabledForTesting());
    REQUIRE(debug_text.queuedGlyphCountForTesting() == 0);
}

TEST_CASE("GameContext debugText is a no-op before the feature registers a pass", "[debug-text]") {
    ensureLogger();
    FastModuleContainer modules;
    GameContext context;

    context.debugText(-1024, -1024, "offscreen");

    auto &debug_text = GET_MODULE(DebugText);
    REQUIRE_FALSE(debug_text.isEnabledForTesting());
    REQUIRE(debug_text.queuedGlyphCountForTesting() == 0);
}

} // namespace Pelican
