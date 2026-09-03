#include "../src/core/loader/scene.hpp"

namespace Pelican::test_support {

void setSceneLoadFaultInjectorForTesting(
    SceneLoader &loader, SceneLoadFaultInjector *injector) noexcept {
    loader.fault_injector = injector;
}

void setDefaultSceneLoadFaultInjectorForTesting(
    SceneLoadFaultInjector *injector) noexcept {
    SceneLoader::default_fault_injector_for_testing = injector;
}

} // namespace Pelican::test_support
