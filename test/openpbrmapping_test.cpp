#include "../src/project/openpbrmapping.hpp"

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

nlohmann::json mappingFixture() {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
                      "openpbr" / "mapping_v1_1_1.json";
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("failed to open " + path.string());
    return nlohmann::json::parse(file);
}

} // namespace

TEST_CASE("OpenPBR mapping fixture exact-pins the supported numeric surface",
          "[openpbr][mapping]") {
    const auto fixture = mappingFixture();
    REQUIRE(fixture.at("schema") == "pelican.openpbr_mapping");
    REQUIRE(fixture.at("version") == 1);
    REQUIRE(fixture.at("openpbr").at("version") == "1.1.1");
    REQUIRE(fixture.at("openpbr").at("tag") == "v1.1.1");
    REQUIRE(fixture.at("openpbr").at("commit") ==
            "f8d6d947dfae4c9b599965a86c22826ea7a8dbfb");

    const auto &parameters = fixture.at("parameters");
    REQUIRE(parameters.size() == 19);
    REQUIRE(parameters.at(0).at("name") == "base_weight");
    REQUIRE(parameters.at(7).at("name") == "specular_ior");
    REQUIRE(parameters.at(8).at("name") == "coat_weight");
    REQUIRE(parameters.at(12).at("name") == "coat_darkening");
    REQUIRE(parameters.at(13).at("name") == "emission_luminance");
    REQUIRE(parameters.at(15).at("name") == "geometry_opacity");
    REQUIRE(parameters.at(16).at("name") == "geometry_normal");
    REQUIRE(parameters.at(16).at("target_texture") == "geometry_normal_map");
    REQUIRE(parameters.at(17).at("name") == "geometry_coat_normal");
    REQUIRE(parameters.at(17).at("target_texture") == "geometry_coat_normal_map");
    REQUIRE(parameters.at(18).at("name") == "alpha_cutoff");

    const auto &numeric = fixture.at("numeric_cases");
    REQUIRE(numeric.at("ior_1_5_f0") == 0.04);
    REQUIRE(numeric.at("coat_ior_1_6_f0").get<double>() > 0.0532);
    REQUIRE(numeric.at("coat_ior_1_6_f0").get<double>() < 0.0533);
    REQUIRE(numeric.at("mask_boundary").at("keep_at_cutoff") == true);
    REQUIRE(numeric.at("mask_boundary").at("discard_below_cutoff") == true);
    REQUIRE(numeric.at("normal_decode").at("encoded") ==
            nlohmann::json::array({0.5, 0.5, 1.0}));
    REQUIRE(numeric.at("normal_decode").at("decoded") ==
            nlohmann::json::array({0.0, 0.0, 1.0}));

    REQUIRE(fixture.at("mappings").at("usd_preview_surface").size() >= 8);
    REQUIRE(fixture.at("mappings").at("materialx_openpbr").size() == 18);
    REQUIRE(fixture.at("mappings").at("gltf").size() >= 12);
    REQUIRE(fixture.at("materialx_allowlist").at("surface_node") ==
            "open_pbr_surface");
    REQUIRE(fixture.at("materialx_allowlist").at("value_nodes") ==
            nlohmann::json::array({"constant", "image"}));
    REQUIRE(fixture.at("materialx_allowlist").at("reject") ==
            nlohmann::json::array({"arbitrary_nodegraph", "unsupported_lobe"}));
}

TEST_CASE("unsupported OpenPBR WARN requires a contributing authored value or connection",
          "[openpbr][warning]") {
    OpenPbrUnsupportedInputObservation observation{
        .prim_path = "/World/Looks/Paint",
        .input = "fuzz_weight",
        .fallback = "0.0",
    };
    REQUIRE_FALSE(makeOpenPbrUnsupportedInputWarning(observation));

    observation.authored = true;
    REQUIRE_FALSE(makeOpenPbrUnsupportedInputWarning(observation));
    observation.authored_non_default = true;
    const auto authored = makeOpenPbrUnsupportedInputWarning(observation);
    REQUIRE(authored);
    REQUIRE(formatOpenPbrUnsupportedInputWarning(*authored) ==
            R"({"code":"OPENPBR_UNSUPPORTED_INPUT","prim_path":"/World/Looks/Paint","input":"fuzz_weight","fallback":"0.0"})");

    observation.authored = false;
    observation.authored_non_default = false;
    observation.connected = true;
    REQUIRE_FALSE(makeOpenPbrUnsupportedInputWarning(observation));
    observation.connection_contributes = true;
    REQUIRE(makeOpenPbrUnsupportedInputWarning(observation));
}

} // namespace Pelican
