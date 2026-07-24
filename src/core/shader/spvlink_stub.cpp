#include "spvlink.hpp"

#include "../build_features.hpp"

namespace Pelican {

SpvLinkResult linkSpirvModules(const SpvLinkRequest &) {
    throwBuildFeatureDisabled(
        "PELICAN_WITH_SPIRV_LINK",
        "experimental SPIR-V linking is unavailable");
}

std::string spvLinkToolchainManifest() {
    return "spirv-link=disabled";
}

} // namespace Pelican
