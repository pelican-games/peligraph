#include "../src/core/material/material.hpp"
#include "../src/core/model/modeltemplate.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

PrimitiveMaterialBindingDocument bindingDocument() {
    return parsePrimitiveMaterialBindingJson(nlohmann::json::parse(R"json(
        {
          "schema": "pelican.material_bindings",
          "version": 1,
          "model": "project://imports/two.glb",
          "bindings": [
            {"usd_path": "/World/First", "mesh": 0, "primitive": 0,
             "material": "second",
             "routing": {"alpha_mode": "opaque", "double_sided": false}},
            {"usd_path": "/World/Second", "mesh": 0, "primitive": 1,
             "material": "first",
             "routing": {"alpha_mode": "blend", "double_sided": true}}
          ]
        }
    )json"));
}

ModelTemplate twoPrimitiveModel() {
    ModelTemplate model;
    model.named_materials = {
        {"first", GlobalMaterialId{10}},
        {"second", GlobalMaterialId{20}},
    };
    model.material_primitives = {
        ModelTemplate::MaterialPrimitives{
            GlobalMaterialId{10},
            {ModelPrimitiveRefInfo{3, 0, 0, false, 0, 0},
             ModelPrimitiveRefInfo{3, 3, 3, false, 0, 1}},
        },
    };
    return model;
}

} // namespace

TEST_CASE("model template consumes whole-model primitive material bindings",
          "[material-binding][model-template]") {
    auto model = twoPrimitiveModel();
    applyPrimitiveMaterialBindings(model, bindingDocument(), "two");

    REQUIRE(model.material_primitives.size() == 2);
    REQUIRE(model.material_primitives[0].material == GlobalMaterialId{20});
    REQUIRE(model.material_primitives[0].primitives.front().primitive_index == 0);
    REQUIRE(model.material_primitives[1].material == GlobalMaterialId{10});
    REQUIRE(model.material_primitives[1].primitives.front().primitive_index == 1);
}

TEST_CASE("model template binding errors name missing material mapping and fragment",
          "[material-binding][model-template][error]") {
    auto missing_mapping = bindingDocument();
    missing_mapping.bindings.pop_back();
    auto model = twoPrimitiveModel();
    REQUIRE_THROWS_WITH(applyPrimitiveMaterialBindings(model, missing_mapping, "two"),
                        Catch::Matchers::ContainsSubstring("two") &&
                            Catch::Matchers::ContainsSubstring("missing a binding") &&
                            Catch::Matchers::ContainsSubstring("primitive 1"));

    auto missing_material = bindingDocument();
    missing_material.bindings.front().material = "absent";
    model = twoPrimitiveModel();
    REQUIRE_THROWS_WITH(applyPrimitiveMaterialBindings(model, missing_material, "two"),
                        Catch::Matchers::ContainsSubstring("/World/First") &&
                            Catch::Matchers::ContainsSubstring("absent") &&
                            Catch::Matchers::ContainsSubstring("missing material"));

    model = twoPrimitiveModel();
    REQUIRE_THROWS_WITH(
        applyPrimitiveMaterialBindings(model, bindingDocument(), "two",
                                       std::string_view{"mesh/Body"}),
        Catch::Matchers::ContainsSubstring("two") &&
            Catch::Matchers::ContainsSubstring("mesh/Body") &&
            Catch::Matchers::ContainsSubstring("whole-model"));
}

TEST_CASE("lowered material binder resolves overridden texture references by declared role",
          "[material-binding][texture-override]") {
    LoweredMaterial lowered;
    lowered.name = "paint";
    lowered.textures = {
        LoweredTextureBinding{"color", 7, "project://color.png",
                              SurfaceTextureRole::color, LoweredTextureView::srgb,
                              MaterialDummyTexture::white},
        LoweredTextureBinding{"normal", 8, "project://normal.png",
                              SurfaceTextureRole::data, LoweredTextureView::unorm,
                              MaterialDummyTexture::flat_normal},
    };
    MaterialInfo destination;
    applyLoweredMaterial(
        destination, lowered,
        [](std::string_view reference, SurfaceTextureRole role) {
            if (reference == "project://color.png" && role == SurfaceTextureRole::color)
                return GlobalTextureId{101};
            if (reference == "project://normal.png" && role == SurfaceTextureRole::data)
                return GlobalTextureId{202};
            throw std::runtime_error("unexpected texture request");
        });
    REQUIRE(destination.custom_textures.size() == 2);
    REQUIRE(destination.custom_textures[0].texture == GlobalTextureId{101});
    REQUIRE(destination.custom_textures[1].texture == GlobalTextureId{202});
}

} // namespace Pelican
