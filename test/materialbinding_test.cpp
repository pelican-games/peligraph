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
        {
            "first",
            GlobalMaterialId{10},
            MaterialVariantRouting{
                MaterialAlphaMode::blend, true},
        },
        {
            "second",
            GlobalMaterialId{20},
            MaterialVariantRouting{
                MaterialAlphaMode::opaque, false},
        },
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

TEST_CASE("MR1 model bindings recover missing materials and uncovered primitives but reject fragments",
          "[material-binding][model-template][error]") {
    auto missing_mapping = bindingDocument();
    missing_mapping.bindings.pop_back();
    auto model = twoPrimitiveModel();
    REQUIRE_NOTHROW(applyPrimitiveMaterialBindings(model, missing_mapping, "two"));
    REQUIRE(model.binding_issues.size() == 1);
    CHECK_FALSE(model.binding_issues[0].row);
    CHECK(model.binding_issues[0].primitive == 1);
    CHECK(model.material_primitives[1].material == GlobalMaterialId{10});

    auto missing_material = bindingDocument();
    missing_material.bindings.front().material = "absent";
    model = twoPrimitiveModel();
    REQUIRE_NOTHROW(applyPrimitiveMaterialBindings(model, missing_material, "two"));
    REQUIRE(model.binding_issues.size() == 1);
    CHECK(model.binding_issues[0].row == 0);
    CHECK(model.binding_issues[0].usd_path == "/World/First");
    CHECK(model.binding_issues[0].material == "absent");
    CHECK(model.material_primitives[0].material == GlobalMaterialId{10});

    model = twoPrimitiveModel();
    REQUIRE_THROWS_WITH(
        applyPrimitiveMaterialBindings(model, bindingDocument(), "two",
                                       std::string_view{"mesh/Body"}),
        Catch::Matchers::ContainsSubstring("two") &&
            Catch::Matchers::ContainsSubstring("mesh/Body") &&
            Catch::Matchers::ContainsSubstring("whole-model"));
}

TEST_CASE("MR1 stale binding rows preserve valid bindings and primitive modes", "[mr1][material-binding]") {
    auto model = twoPrimitiveModel();
    model.material_primitives[0].primitives.resize(1);
    model.material_primitives[0].primitives[0].skinned = true;
    REQUIRE_NOTHROW(applyPrimitiveMaterialBindings(model, bindingDocument(), "two"));
    REQUIRE(model.binding_issues.size() == 1);
    CHECK(model.binding_issues[0].row == 1);
    CHECK(model.binding_issues[0].reason.find("targets missing") != std::string::npos);
    CHECK(model.material_primitives[0].material == GlobalMaterialId{20});
    CHECK(model.material_primitives[0].primitives[0].skinned);
    model = twoPrimitiveModel();
    applyPrimitiveMaterialBindings(model, bindingDocument(), "two");
    CHECK(model.binding_issues.empty());
}

TEST_CASE("binding routing is consumed and project names extend the resolution domain",
          "[material-binding][model-template][wp240c]") {
    auto mismatched = bindingDocument();
    mismatched.bindings.front().routing = {
        MaterialAlphaMode::blend, true};
    auto model = twoPrimitiveModel();
    REQUIRE_THROWS_WITH(
        applyPrimitiveMaterialBindings(
            model, mismatched, "two"),
        Catch::Matchers::ContainsSubstring(
            "/World/First") &&
            Catch::Matchers::ContainsSubstring(
                "second") &&
            Catch::Matchers::ContainsSubstring(
                "routing"));

    model = twoPrimitiveModel();
    model.named_materials.erase(
        model.named_materials.begin() + 1);
    const std::array project_materials{
        ModelTemplate::NamedMaterial{
            "second", GlobalMaterialId{30},
            MaterialVariantRouting{
                MaterialAlphaMode::opaque, false}},
    };
    applyPrimitiveMaterialBindings(
        model, bindingDocument(), "two",
        std::nullopt, project_materials);
    REQUIRE(model.material_primitives.size() == 2);
    REQUIRE(model.material_primitives[0].material ==
            GlobalMaterialId{30});
    REQUIRE(model.material_primitives[1].material ==
            GlobalMaterialId{10});
}

TEST_CASE("glTF and project material name collision is a named hard error",
          "[material-binding][model-template][wp240c][error]") {
    const auto model = twoPrimitiveModel();
    const std::array project_materials{
        ModelTemplate::NamedMaterial{
            "first", GlobalMaterialId{99},
            MaterialVariantRouting{
                MaterialAlphaMode::blend, true}},
    };
    REQUIRE_THROWS_WITH(
        validateNamedMaterialResolutionDomains(
            model, project_materials, "two"),
        Catch::Matchers::ContainsSubstring("two") &&
            Catch::Matchers::ContainsSubstring("first") &&
            Catch::Matchers::ContainsSubstring("glTF") &&
            Catch::Matchers::ContainsSubstring("project"));
}

TEST_CASE("lowered material binder resolves overridden texture references by declared role",
          "[material-binding][texture-override]") {
    LoweredMaterial lowered;
    lowered.name = "paint";
    lowered.tags = {"character", "outline"};
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
    REQUIRE(destination.tags == lowered.tags);
    REQUIRE(destination.custom_textures[0].texture == GlobalTextureId{101});
    REQUIRE(destination.custom_textures[1].texture == GlobalTextureId{202});
    REQUIRE(destination.shader_contract ==
            MaterialShaderContract::legacy_gbuffer_v1);

    lowered.route = MaterialRouteClass::forward_opaque;
    applyLoweredMaterialForRoute(destination, lowered);
    REQUIRE(destination.shader_contract ==
            MaterialShaderContract::forward_scene_color_v1);

    lowered.screen_inputs = {"opaque_color"};
    const auto logical_types = makeBuiltinLogicalTypeRegistry();
    lowered.screen_input_contracts = {
        makeBuiltinMaterialScreenInputContract(logical_types,
                                               "opaque_color")};
    applyLoweredMaterialForRoute(destination, lowered);
    REQUIRE(destination.screen_inputs == lowered.screen_input_contracts);
    REQUIRE(destination.screen_inputs.front().footprint.kind ==
            LogicalReadFootprintKind::neighborhood);
}

} // namespace Pelican

namespace Pelican {
TEST_CASE("SK1 material bindings retain primitive skin mode and refuse unbaked VAT", "[material][sk1]") {
    auto model = twoPrimitiveModel();
    model.material_primitives[0].primitives[0].skinned = true;
    const auto bindings = bindingDocument();
    applyPrimitiveMaterialBindings(model, bindings, "mixed");
    REQUIRE(model.material_primitives[0].primitives[0].skinned);
    REQUIRE_FALSE(model.material_primitives[1].primitives[0].skinned);
    REQUIRE(model.material_primitives[0].material == GlobalMaterialId{20});
    model = twoPrimitiveModel();
    model.material_primitives[0].primitives[0].vat_deformed = true;
    REQUIRE_THROWS_WITH(applyPrimitiveMaterialBindings(model, bindings, "vat"),
        Catch::Matchers::ContainsSubstring("VAT primitives keep their baked material"));
    REQUIRE(model.material_primitives.size() == 1);
    auto keep = bindings;
    keep.bindings[0].material = "baked-private";
    applyPrimitiveMaterialBindings(model, keep, "vat", std::nullopt, {},
        [](const auto &binding) { return binding.material == "baked-private"; });
    REQUIRE(model.material_primitives[0].material == GlobalMaterialId{10});
}

TEST_CASE("MO1 object_material coordinates are checked across material groups and vertex modes",
          "[mo1][object_material]") {
    auto model = twoPrimitiveModel();
    model.material_primitives.push_back({GlobalMaterialId{20},
        {ModelPrimitiveRefInfo{3, 6, 0, true, 1, 0}}});
    CHECK(model.hasPrimitive(0, 0));
    CHECK(model.hasPrimitive(0, 1));
    CHECK(model.hasPrimitive(1, 0));
    CHECK_FALSE(model.hasPrimitive(1, 1));
    CHECK_FALSE(model.hasPrimitive(2, 0));
    model.material_primitives.erase(model.material_primitives.begin() + 1);
    CHECK_FALSE(model.hasPrimitive(1, 0));
    model.material_primitives.clear();
    CHECK_FALSE(model.hasPrimitive(0, 0));
}
} // namespace Pelican
