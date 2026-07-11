#include "gltf_scene_fixture.hpp"

#include <filesystem>
#include <iostream>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "usage: pelican_test_gltf_scene_fixture_writer <output.glb>\n";
        return 2;
    }
    Pelican::TestGltfSceneFixture::writeGlb(std::filesystem::path{argv[1]}, true);
    return 0;
}
