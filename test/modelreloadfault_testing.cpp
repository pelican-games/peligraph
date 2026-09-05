// Test-only translation unit. It is linked into the model hot-reload test
// executable and must never appear in pelican_core sources; the CTest
// `wp380c_production_has_no_fault_setter_symbol` guard enforces both halves.
#include "../src/core/renderer/polygoninstancecontainer.hpp"

namespace Pelican::test_support {

void setRayQueryGeometryGenerationForTesting(
    PolygonInstanceContainer &instances,
    std::uint64_t generation) noexcept {
    instances.ray_query_geometry_generation = generation;
}

} // namespace Pelican::test_support
