#include "combination_fixture.hpp"
#include "../src/project/assetdataformat.hpp"
#include "../src/project/materialformat.hpp"
#include "../src/project/materiallowering.hpp"
#include "../src/project/surfaceformat.hpp"

using namespace Pelican;
using namespace Pelican::TestSupport::CB1;

TEST_CASE("CB1 all twelve generated material fixtures satisfy authoring contracts", "[cb1][cpu][fixture]") {
    const auto read = [](const std::filesystem::path &path) {
        std::ifstream input{path, std::ios::binary};
        REQUIRE(input.good());
        return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    };
    for (int row = 1; row <= 12; ++row) {
        CAPTURE(row);
        CombinationProject project{row, true};
        const auto assets = parseAssetDataFormatJson(Json::parse(read(project.root / "assets.json")));
        CHECK(assets.models.size() == 2);
        const auto bindings = parsePrimitiveMaterialBindingJson(Json::parse(read(project.root / "binding.json")));
        CHECK(bindings.bindings.size() == ((row == 4 || row == 5) ? 2 : 1));
        const auto json = Json::parse(read(project.root / "paint.material.json"));
        MaterialSurfaceCatalog surfaces;
        const auto add = [&](const std::string &reference) {
            const auto path = reference.starts_with("project://") ? project.root / reference.substr(10) :
                std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "src/core/resources" / reference.substr(9);
            surfaces.emplace(reference, parseSurfaceFormat(read(path), reference));
        };
        for (const auto &material : json.at("materials")) {
            add(material.at("surface").get<std::string>());
            if (material.contains("variants")) for (const auto &variant : material.at("variants"))
                add(variant.at("surface").get<std::string>());
        }
        const auto document = parseMaterialFormatJson(json, surfaces);
        for (const auto &material : document.materials) {
            const auto lowered = lowerMaterial(material, surfaces.at(*material.surface));
            CHECK(lowered.routing.has_value());
            const auto variants = lowerMaterialVariants(material, surfaces);
            CHECK(variants.size() == (row == 2 ? 1 : 0));
        }
        const auto graph = Json::parse(read(project.root / "hybrid.json"));
        CHECK(graph.at("schema") == "pelican.graph");
        CHECK(graph.at("version") == 1);
    }
}
