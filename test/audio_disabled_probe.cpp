#include "../src/core/build_features.hpp"
#include "../src/core/userpublic/gamecontext.hpp"

#include <exception>
#include <iostream>

// run_build_units_smoke.cmake runs this in the PELICAN_WITH_AUDIO=OFF build.
// PG-2: the facade fails with BuildFeatureCallDisabledError, so the probe
// prints its code and feature beside the prose message; a code-less
// BuildFeatureDisabledError prints only the message and fails the smoke.
int main() {
    try {
        Pelican::GameContext{}.audio().playSound("project://missing.wav");
    } catch (const Pelican::BuildFeatureCallDisabledError &error) {
        std::cerr << error.what() << std::endl;
        std::cerr << "PG2_FACADE code=" << error.code()
                  << " feature=" << error.feature() << std::endl;
        return 1;
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    return 0;
}
