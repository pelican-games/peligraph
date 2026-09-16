#pragma once

#include "../src/project/graphmodule.hpp"

namespace Pelican {

// pelican.test.opaque exists only so a test can show that a module's own type
// and operations reach the common graph through registration alone, and that
// the same document is rejected by name once the module is gone. Production
// never links this translation unit: run_wp380c_no_production_fault_setter.cmake
// fails if registerTestOpaqueGraphModule appears in a shipped archive.
enum class TestOpaqueModuleTypes {
    registered,
    // Operations without their type, to show graph.type_unregistered@1.
    omitted,
};

void registerTestOpaqueGraphModule(
    GraphModuleRegistry &registry,
    TestOpaqueModuleTypes types = TestOpaqueModuleTypes::registered);

} // namespace Pelican
