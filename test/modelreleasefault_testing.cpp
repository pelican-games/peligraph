#include "../src/core/model/vertbufcontainer.hpp"
#include "../src/core/material/materialcontainer.hpp"

namespace Pelican::test_support {

void setMorphMetadataReleaseFailureForTesting(
    VertBufContainer &geometry, bool enabled) noexcept {
    geometry.fail_morph_metadata_release_for_testing = enabled;
}

void setMaterialReleaseFailureForTesting(
    MaterialContainer &materials, bool enabled) noexcept {
    materials.fail_material_release_for_testing = enabled;
}

void setTextureReleaseFailureForTesting(
    MaterialContainer &materials, bool enabled) noexcept {
    materials.fail_texture_release_for_testing = enabled;
}

} // namespace Pelican::test_support
