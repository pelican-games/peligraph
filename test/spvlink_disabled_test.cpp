#include "../src/core/shader/spvlink.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace Pelican;

TEST_CASE(
    "SPIR-V linker build unit reports an explicit disabled error",
    "[shader][spv-link][build-unit]") {
    REQUIRE_THROWS_WITH(
        linkSpirvModules({}),
        "This binary was built with PELICAN_WITH_SPIRV_LINK=OFF: "
        "experimental SPIR-V linking is unavailable");
    REQUIRE(spvLinkToolchainManifest() ==
            "spirv-link=disabled");
}
