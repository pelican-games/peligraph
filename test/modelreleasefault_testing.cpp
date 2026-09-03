#include "../src/core/model/vertbufcontainer.hpp"

namespace Pelican::test_support {

void setMorphMetadataReleaseFailureForTesting(
    VertBufContainer &geometry, bool enabled) noexcept {
    geometry.fail_morph_metadata_release_for_testing = enabled;
}

} // namespace Pelican::test_support
