#include "../src/core/model/modeltemplate.hpp"
#include "../src/core/model/vrmsemantic.hpp"
#include "vrm_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <tiny_gltf.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <type_traits>

namespace Pelican {
namespace {

tinygltf::Model loadFixture(TestVrmFixture::Kind kind) {
    const auto bytes = TestVrmFixture::makeGlb(kind);
    tinygltf::TinyGLTF loader;
    tinygltf::Model model;
    std::string errors;
    std::string warnings;
    if (!loader.LoadBinaryFromMemory(&model, &errors, &warnings, bytes.data(),
                                     static_cast<unsigned int>(bytes.size()))) {
        throw std::runtime_error("fixture GLB failed to load: " + errors);
    }
    return model;
}

std::string readText(const std::filesystem::path &path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) throw std::runtime_error("could not open fixture: " + path.string());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

const VrmHumanBone &bone(const VrmSemanticData &semantic, std::string_view name) {
    const auto found = std::find_if(semantic.human_bones.begin(), semantic.human_bones.end(),
                                    [&](const VrmHumanBone &item) { return item.name == name; });
    if (found == semantic.human_bones.end()) throw std::runtime_error("bone not found");
    return *found;
}

} // namespace

static_assert(std::is_same_v<decltype(ModelTemplate::vrm_semantic),
                             std::shared_ptr<const VrmSemanticData>>);

TEST_CASE("VRM 1.0 semantic decode retains typed metadata and canonical dump", "[vrm]") {
    const auto model = loadFixture(TestVrmFixture::Kind::full);
    const auto decoded = decodeVrmSemantic(model, "full.vrm");
    REQUIRE(decoded.semantic);
    REQUIRE(decoded.diagnostics.empty());
    REQUIRE(decoded.semantic->spec_version == "1.0");
    REQUIRE(decoded.semantic->human_bones.size() == 16);
    REQUIRE(bone(*decoded.semantic, "hips").node == 0);
    REQUIRE(bone(*decoded.semantic, "chest").required == false);
    REQUIRE(decoded.semantic->preset_expressions.at("happy").is_binary);
    REQUIRE(decoded.semantic->preset_expressions.at("happy").morph_target_binds.at(0).node == 2);
    REQUIRE(decoded.semantic->preset_expressions.at("happy").material_color_binds.at(0).material == 0);
    REQUIRE(decoded.semantic->preset_expressions.at("happy").texture_transform_binds.at(0).scale[0] == 2.0);
    REQUIRE(decoded.semantic->custom_expressions.contains("smileWide"));
    REQUIRE(decoded.semantic->look_at);
    REQUIRE(decoded.semantic->look_at->type == "expression");
    REQUIRE(decoded.semantic->first_person);
    REQUIRE(decoded.semantic->first_person->mesh_annotations.at(0).node == 2);
    REQUIRE(decoded.semantic->node_constraints.size() == 1);
    REQUIRE(decoded.semantic->node_constraints.at(0).source_node == 0);

    const auto expected = readText(std::filesystem::path{PELICAN_TEST_SOURCE_DIR} /
                                   "test/fixtures/vrm_semantic/canonical.json");
    REQUIRE(dumpVrmSemanticCanonical(*decoded.semantic) == expected);
    const auto decoded_again = decodeVrmSemantic(model, "renamed.vrm");
    REQUIRE(dumpVrmSemanticCanonical(*decoded_again.semantic) == expected);
}

TEST_CASE("VRM humanoid optional and unknown bones follow the 1.0 gate", "[vrm]") {
    SECTION("optional bones may be absent") {
        const auto decoded = decodeVrmSemantic(
            loadFixture(TestVrmFixture::Kind::optional_bones_missing), "optional.vrm");
        REQUIRE(decoded.semantic);
        REQUIRE(decoded.semantic->human_bones.size() == 15);
        REQUIRE(std::none_of(decoded.semantic->human_bones.begin(),
                             decoded.semantic->human_bones.end(),
                             [](const VrmHumanBone &item) { return item.name == "chest"; }));
    }
    SECTION("unknown bones are retained with one warning") {
        const auto decoded = decodeVrmSemantic(loadFixture(TestVrmFixture::Kind::unknown_bone),
                                               "unknown.vrm");
        REQUIRE(decoded.semantic);
        REQUIRE(decoded.diagnostics.size() == 1);
        REQUIRE(decoded.diagnostics[0].severity == VrmDiagnosticSeverity::warning);
        REQUIRE_THAT(decoded.diagnostics[0].message,
                     Catch::Matchers::ContainsSubstring("unknown humanoid bone 'tail'"));
        REQUIRE_FALSE(bone(*decoded.semantic, "tail").recognized);
    }
}

TEST_CASE("VRM humanoid rejects duplicate, missing, and invalid node assignments", "[vrm]") {
    REQUIRE_THROWS_WITH(
        decodeVrmSemantic(loadFixture(TestVrmFixture::Kind::duplicate_bone), "duplicate.vrm"),
        Catch::Matchers::ContainsSubstring("duplicates bone") &&
            Catch::Matchers::ContainsSubstring("glTF node 0"));
    REQUIRE_THROWS_WITH(
        decodeVrmSemantic(loadFixture(TestVrmFixture::Kind::missing_required_bone), "missing.vrm"),
        Catch::Matchers::ContainsSubstring("missing required bone 'rightHand'"));
    REQUIRE_THROWS_WITH(
        decodeVrmSemantic(loadFixture(TestVrmFixture::Kind::invalid_node), "node.vrm"),
        Catch::Matchers::ContainsSubstring("invalid glTF node index 999"));
}

TEST_CASE("VRM decoder ignores unsupported and absent semantic versions", "[vrm]") {
    SECTION("unsupported VRMC_vrm version") {
        const auto decoded = decodeVrmSemantic(
            loadFixture(TestVrmFixture::Kind::unsupported_version), "future.vrm");
        REQUIRE_FALSE(decoded.semantic);
        REQUIRE(decoded.diagnostics.size() == 1);
        REQUIRE_THAT(decoded.diagnostics[0].message,
                     Catch::Matchers::ContainsSubstring("future.vrm") &&
                         Catch::Matchers::ContainsSubstring("specVersion '1.1'"));
    }
    SECTION("plain GLB") {
        const auto decoded = decodeVrmSemantic(loadFixture(TestVrmFixture::Kind::plain_glb),
                                               "plain.glb");
        REQUIRE_FALSE(decoded.semantic);
        REQUIRE(decoded.diagnostics.empty());
    }
    SECTION("VRM 0.x remains plain GLB and emits one info diagnostic") {
        const auto decoded = decodeVrmSemantic(loadFixture(TestVrmFixture::Kind::vrm0),
                                               "AliciaSolid.vrm");
        REQUIRE_FALSE(decoded.semantic);
        REQUIRE(decoded.diagnostics.size() == 1);
        REQUIRE(decoded.diagnostics[0].severity == VrmDiagnosticSeverity::info);
        REQUIRE_THAT(decoded.diagnostics[0].message,
                     Catch::Matchers::ContainsSubstring("VRM 0.x semantic is unsupported"));
    }
    SECTION("unsupported constraint version does not discard VRMC_vrm") {
        const auto decoded = decodeVrmSemantic(
            loadFixture(TestVrmFixture::Kind::unsupported_constraint_version), "constraint.vrm");
        REQUIRE(decoded.semantic);
        REQUIRE(decoded.semantic->node_constraints.empty());
        REQUIRE(decoded.diagnostics.size() == 1);
        REQUIRE_THAT(decoded.diagnostics[0].message,
                     Catch::Matchers::ContainsSubstring("VRMC_node_constraint specVersion '2.0'"));
    }
}

TEST_CASE("VRM humanoid maps glTF node indices into the A1 rig name layout", "[vrm]") {
    const auto model = loadFixture(TestVrmFixture::Kind::full);
    const auto semantic = decodeVrmSemantic(model, "map.vrm").semantic;
    std::vector<std::string> gltf_names;
    for (const auto &node : model.nodes) gltf_names.push_back(node.name);
    auto rig_names = gltf_names;
    std::reverse(rig_names.begin(), rig_names.end());
    const auto mapping = mapVrmHumanoidToRig(*semantic, gltf_names, rig_names);
    const auto hips = std::find_if(mapping.begin(), mapping.end(),
                                   [](const VrmRigBoneMapping &item) {
                                       return item.bone == "hips";
                                   });
    REQUIRE(hips != mapping.end());
    REQUIRE(hips->gltf_node == 0);
    REQUIRE(hips->rig_node == static_cast<int>(rig_names.size() - 1));

    rig_names.push_back("hipsNode");
    REQUIRE_THROWS_WITH(mapVrmHumanoidToRig(*semantic, gltf_names, rig_names),
                        Catch::Matchers::ContainsSubstring("ambiguous rig node name 'hipsNode'"));
}

} // namespace Pelican
