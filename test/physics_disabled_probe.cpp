// The PELICAN_WITH_PHYSICS=OFF counterpart of audio_disabled_probe.cpp.
//
// Before C-8 every GameContext physics query answered an empty vector in an
// OFF build, so "no physics in this binary" and "nothing was hit" were the same
// answer. run_build_units_smoke.cmake runs this in the OFF build and requires
// the named BuildFeatureDisabledError instead.
#include "../src/core/userpublic/gamecontext.hpp"

#include <exception>
#include <iostream>

int main() {
    try {
        const Pelican::phys::Ray ray{};
        const auto hits = Pelican::GameContext{}.physics().raycastAll(ray);
        std::cerr << "raycastAll returned " << hits.size() << " hits" << std::endl;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    return 0;
}
