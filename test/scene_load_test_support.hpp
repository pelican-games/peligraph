#pragma once

#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/scene.hpp"

#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <string_view>
#include <utility>

namespace Pelican::test_support {

inline void loadScene(SceneLoader &loader, SceneId scene_id) {
    auto &config =
        FastModuleContainer::get<ProjectBasicConfig>();
    auto request = SceneLoadRequest::borrowed(
        std::move(scene_id), config.sceneProjectionState());
    try {
        (void)requireSceneLoadCommitted(
            loader.load(std::move(request)));
    } catch (const std::exception &error) {
        // A scene load creates a Vulkan instance. A machine with no Vulkan driver at all (a hosted CI
        // runner) cannot do that: vk::createInstanceUnique reports ErrorIncompatibleDriver. The case is
        // skipped there, by name (test/ci/cpu_skip_allowlist.txt); every other failure is still a failure.
        if (std::string_view{error.what()}.find("ErrorIncompatibleDriver") != std::string_view::npos) {
            SKIP("no Vulkan driver on this machine: a scene load creates a Vulkan instance");
        }
        throw;
    }
}

} // namespace Pelican::test_support
