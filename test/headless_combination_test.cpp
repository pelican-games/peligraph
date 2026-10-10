// CB-1: one independently selectable case per combination. Failed captures
// are retained under PELICAN_CB1_ARTIFACT_DIR (or the CTest working directory).
#include "render_coverage_test_support.hpp"
#include "combination_fixture.hpp"
#include "scene_load_test_support.hpp"
#include "polygoninstancecontainer_testaccess.hpp"
#include "renderer_testaccess.hpp"
#include "vulkan_test_support.hpp"
#include "../src/core/animation/vrmapplication.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/appflow/teardown.hpp"
#include "../src/core/asset/model.hpp"
#include "../src/core/communication/rpcserver.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/light/lightcontainer.hpp"
#include "../src/core/log.hpp"
#include "../src/core/material/materialcontainer.hpp"
#include "../src/core/material/materialdrawlooptable.hpp"
#include "../src/core/material/projectmaterialasset.hpp"
#include "../src/core/model/gltf.hpp"
#include "../src/core/renderer/camera.hpp"
#include "../src/core/renderer/materialrender.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/core/watch/reloadservice.hpp"
#include <catch2/catch_test_macros.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <set>

namespace Pelican {
namespace {
#if PELICAN_RUNTIME_SHADER_COMPILER
using Json = nlohmann::json;
using TestSupport::RenderCoverage;

using namespace TestSupport::CB1;
struct CombinationFixture : CombinationProject {
    using CombinationProject::CombinationProject;
    void configure() const {
        GET_MODULE(ProjectSource).setSourceByFile((root / "project.json").string());
        GET_MODULE(PathResolver).setup(root, false);
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.strict_assets = true;
        launch.headless = true; launch.headless_extent = vk::Extent2D{128,128}; launch.headless_frames = 1;
        GET_MODULE(EngineTime).setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
    }
    bool compare(const RenderCoverage &a, const RenderCoverage &b, std::string_view pass,
                 bool known_red_candidate = false) const {
        const auto result = TestSupport::compareCoverage(a, b);
        const auto name = std::string{pass};
        TestSupport::writeCoverageDifference(root / (name + "_difference.ppm"), a, b);
        TestSupport::writeCoverageDifference(root / (name + "_a.ppm"), a, a);
        TestSupport::writeCoverageDifference(root / (name + "_b.ppm"), b, b);
        cbWrite(root / (name + "_comparison.json"), Json{{"different", result.different},
            {"pixels", a.pixels.size()}, {"a_count", a.count()}, {"b_count", b.count()},
            {"a_bbox", a.bbox()}, {"b_bbox", b.bbox()}, {"equivalent", result.equivalent()}}.dump(2));
        INFO("CB1 row " << row << " pass " << name << " artifacts " << root.string());
        REQUIRE(result.nonempty);
        if (!known_red_candidate) {
            CHECK(result.within_tolerance);
            CHECK(result.equal_bbox); CHECK(result.within_one_pixel);
        }
        return result.equivalent();
    }
};

// QD-1 policy: only an observed, ticketed product failure becomes a skip.
// Run the reproduction and ordinary checks on every invocation.
void cbKnownRed(const CombinationFixture &fixture, std::string_view ticket, const Json &evidence) {
    cbWrite(fixture.root / "known_red.json", Json{{"status", "known_red"},
        {"ticket", ticket}, {"evidence", evidence}}.dump(2));
    SKIP("KNOWN_RED " << ticket << " (test/CB1_FINDINGS.md); " << evidence.dump());
}

RenderCoverage cbDepth(std::string_view name) {
    const auto image = RendererTestAccess::readRenderTarget(GET_MODULE(Renderer), std::string{name}, {});
    REQUIRE(image.format == vk::Format::eD32Sfloat);
    std::vector<float> values(image.bytes.size() / sizeof(float));
    std::memcpy(values.data(), image.bytes.data(), image.bytes.size());
    return TestSupport::depthCoverage(values, image.extent.width, image.extent.height);
}

void cbCamera() {
    auto &camera = GET_MODULE(Camera);
    camera.setPos({0,1,4}); camera.setDir({0,0,-1}); camera.setUp({0,1,0});
}

void cbPose(ModelInstanceId id, const ModelTemplate &model, bool deformed = true) {
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    if (model.skeletal && std::any_of(model.material_primitives.begin(), model.material_primitives.end(),
        [](const auto &group) { return std::any_of(group.primitives.begin(), group.primitives.end(), [](const auto &p) { return p.skinned; }); })) {
        const std::array palette{glm::mat4{1}, glm::translate(glm::mat4{1}, glm::vec3{0,1,0}) *
            glm::rotate(glm::mat4{1}, glm::radians(deformed ? 35.0F : 0.0F), glm::vec3{0,0,1}) *
            glm::translate(glm::mat4{1}, glm::vec3{0,-1,0})};
        instances.setSkinningPalette(id, palette);
    }
    if (model.morph_targets && !model.morph_targets->default_weights.empty()) {
        auto weights = model.morph_targets->default_weights;
        weights[0] = deformed ? 0.35F : 0.0F;
        if (model.vrm_semantic) {
            Vrm::ExpressionInputSnapshot input;
            input.expression_weights = {{"happy", deformed ? 1.0F : 0.0F}};
            Vrm::ResolvedExpressionFrame expression;
            REQUIRE(Vrm::resolveExpressionFrame(*model.vrm_semantic, model.morph_targets.get(),
                model.material_initial_values.get(), input, expression) == Animation::Status::ok);
            weights = expression.morph_weights;
            for (const auto &source : expression.material_overrides) {
                PublishMaterialInstanceAbsoluteOverrideDescV2 frame;
                frame.instance = instances.animationInstance(id); frame.frame_revision = 1;
                frame.source_material_index = source.source_material_index;
                frame.values.mask = source.mask;
                frame.values.base_color_factor = source.base_color_factor;
                frame.values.emissive_factor = source.emissive_factor;
                frame.values.uv_offset = source.uv_offset; frame.values.uv_scale = source.uv_scale;
                frame.values.uv_rotation = source.uv_rotation;
                frame.flags = materialOverrideCommitResetHistory;
                REQUIRE(instances.publishMaterialInstanceAbsoluteOverride(id, frame) == Animation::Status::ok);
            }
        }
        REQUIRE(instances.publishMorphWeightFrame(id, {.layout_generation = model.morph_targets->generation,
            .frame_revision = 1, .weights = weights.data(), .weight_count = static_cast<std::uint32_t>(weights.size()),
            .flags = morphCommitResetHistory}) == Animation::Status::ok);
    }
}

using Capture = std::map<std::string, RenderCoverage>;
Capture cbRead(int row) {
    GET_MODULE(Renderer).render(); GET_MODULE(VulkanManageCore).waitIdle();
    Capture result{{"main", cbDepth("scene_depth")}};
    if (row == 1 || row == 6 || row == 8) result["shadow"] = cbDepth("cb_shadow");
    if (row == 8) {
        const auto image = RendererTestAccess::readRenderTarget(GET_MODULE(Renderer), "cb_velocity", {});
        REQUIRE(image.format == vk::Format::eR16G16Sfloat);
        std::vector<std::uint16_t> values(image.bytes.size() / sizeof(std::uint16_t));
        std::memcpy(values.data(), image.bytes.data(), image.bytes.size());
        result["velocity"] = TestSupport::velocityCoverage(values, image.extent.width, image.extent.height);
        const auto picking = RendererTestAccess::readRenderTarget(GET_MODULE(Renderer), "cb_picking", {});
        REQUIRE(picking.format == vk::Format::eR32Uint);
        RenderCoverage mask{picking.extent.width, picking.extent.height, {}};
        for (std::size_t i = 0; i < picking.bytes.size(); i += 4) {
            std::uint32_t value; std::memcpy(&value, picking.bytes.data() + i, 4); mask.pixels.push_back(value != 0);
        }
        result["picking"] = std::move(mask);
    }
    if (row == 3) {
        const auto image = RendererTestAccess::readRenderTarget(GET_MODULE(Renderer), "lit_color", {});
        REQUIRE(image.format == vk::Format::eR16G16B16A16Sfloat);
        RenderCoverage mask{image.extent.width, image.extent.height, {}};
        for (std::size_t i = 0; i < image.bytes.size(); i += 8) {
            std::array<std::uint16_t, 4> rgba;
            std::memcpy(rgba.data(), image.bytes.data() + i, 8);
            mask.pixels.push_back((rgba[0] & 0x7fff) || (rgba[1] & 0x7fff) || (rgba[2] & 0x7fff));
        }
        result["color_support"] = std::move(mask);
    }
    if (row == 11) {
        const auto image = RendererTestAccess::readR8RenderTarget(GET_MODULE(Renderer), "rt_shadow_mask");
        RenderCoverage mask{image.extent.width, image.extent.height, {}};
        for (auto value : image.pixels) mask.pixels.push_back(value < 128);
        result["ray_query_shadow"] = std::move(mask);
    }
    return result;
}

void cbSaveRoundtrip(const CombinationFixture &fixture) {
    const auto render = [&](bool save) {
        FastModuleContainer modules;
        RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
        fixture.configure(); TestSupport::requireVulkanDevice("CB1 requires Vulkan");
        (void)GET_MODULE(Renderer); cbCamera();
        GET_MODULE(ECSPredefinedRegistration).reg();
        test_support::loadScene(GET_MODULE(SceneLoader), "combined");
        // Scene publication resets the free camera when no scene camera exists.
        cbCamera();
        auto &instances = GET_MODULE(PolygonInstanceContainer);
        const auto paint = GET_MODULE(ProjectMaterialAssetContainer).materialByName("Paint");
        const auto &items = PolygonInstanceTestAccess::drawItems(instances);
        REQUIRE(items.size() == 1);
        REQUIRE(items.front().pipeline_material_key.material == paint);
        const auto result = cbRead(12);
        if (save) {
            std::istringstream input; std::ostringstream output; EngineRpcEndpoint endpoint{input, output};
            const auto saved = Json::parse(endpoint.processLine(Json{{"jsonrpc", "2.0"}, {"id", 1},
                {"method", "save_scene"}, {"params", Json::object()}}.dump()));
            INFO(saved.dump()); REQUIRE(saved.contains("result"));
            REQUIRE(saved["result"]["byte_count"].get<std::uint64_t>() > 0);
            cbWrite(fixture.root / "project.json", GET_MODULE(ProjectSource).loadSource());
        }
        return result;
    };
    const auto a = render(true), b = render(false);
    fixture.compare(a.at("main"), b.at("main"), "main");
}

void runCombination(const CombinationFixture &fixture) {
    const auto row = fixture.row;
    if (row == 12) { cbSaveRoundtrip(fixture); return; }
    FastModuleContainer modules;
    RuntimeTeardownGuard teardown{RuntimeTeardownMode::terminal_shutdown};
    fixture.configure();
    TestSupport::requireVulkanDevice("CB1 requires Vulkan");
    if (row == 2) {
        auto rules = std::make_shared<MaterialDrawLoopRules>();
        GraphDrawLoop loop; loop.loop = "cb_skinned_outline"; loop.pass = "forward_opaque";
        loop.contract = MaterialPassContract::forward_opaque_v1;
        GraphLoopDrawCondition condition; condition.skinned = true;
        // The glTF control has no outline variant; only Paint declares it.
        condition.material = "paint.material.json#Paint";
        const auto material_document = Json::parse(std::ifstream{fixture.root / "paint.material.json"});
        REQUIRE(material_document.at("materials").at(0).at("variants").contains("outline"));
        loop.branches.push_back({condition, MaterialDrawStepList{MaterialDrawStep::material(), MaterialDrawStep::named("outline")}});
        loop.branches.push_back({{}, MaterialDrawStepList{MaterialDrawStep::material()}});
        rules->passes["forward_opaque"] = makeGraphDrawLoopPassRule(std::move(loop));
        GET_MODULE(FrameGraphRuntimeContainer).setMaterialDrawLoopRuleSource(
            [rules](const RendererRuntimeGeneration &) { return rules; });
    }
    auto &renderer = GET_MODULE(Renderer);
    if (row == 11) {
        REQUIRE(GET_MODULE(VulkanManageCore).getRuntimeCapabilities().ray_query);
        GET_MODULE(LightContainer).load({LightLoadEntry{.name = "CB1 directional",
            .component = LightCodecData{.type = LightCodecType::Directional,
                .direction = {0.5F,0,-1}, .intensity = 4, .color = {1,1,1}}}});
    }
    cbCamera();
    auto &models = GET_MODULE(ModelAssetContainer);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto original = models.getModelTemplateByName("character");
    auto painted = models.getModelTemplateByName("painted");
    const auto paint = GET_MODULE(ProjectMaterialAssetContainer).materialByName("Paint");
    for (const auto &group : painted.material_primitives) REQUIRE(group.material == paint);
    if (row == 7) REQUIRE(original.vrm_semantic != nullptr);
    std::vector<MaterialDrawRecord> records;
    GET_MODULE(MaterialRenderer).recordMaterialDrawsForTesting(&records);
    struct ResetRecords { ~ResetRecords() { GET_MODULE(MaterialRenderer).recordMaterialDrawsForTesting(nullptr); } } reset;
    const auto capture = [&](const ModelTemplate &model, bool deformed = true, float x = 0.0F) {
        std::optional<ModelInstanceId> receiver;
        if (row == 11) {
            receiver = instances.placeModelInstance(original);
            instances.setTrs(*receiver, {0,-0.5F,-0.5F}, glm::quat{1,0,0,0}, {4,1.5F,1});
        }
        const auto id = instances.placeModelInstance(model); cbPose(id, model, deformed);
        instances.setTrs(id, {x,0,0}, glm::quat{1,0,0,0}, {1,1,1});
        if (row == 8) {
            // Same final pose, nonzero previous/current object motion in both runs.
            instances.setTrs(id, {x - 0.1F,0,0}, glm::quat{1,0,0,0}, {1,1,1});
            renderer.render(); GET_MODULE(VulkanManageCore).waitIdle();
            instances.setTrs(id, {x,0,0}, glm::quat{1,0,0,0}, {1,1,1});
        }
        records.clear(); auto result = cbRead(row);
        REQUIRE(instances.removeModelInstance(id));
        if (receiver) REQUIRE(instances.removeModelInstance(*receiver));
        return result;
    };
    Capture a, b;
    if (row == 4 || row == 5) {
        GET_MODULE(ECSPredefinedRegistration).reg();
        const auto sceneCapture = [&](const char *scene) {
            test_support::loadScene(GET_MODULE(SceneLoader), scene);
            cbCamera();
            std::set<ModelInstanceId> ids;
            for (const auto &item : PolygonInstanceTestAccess::drawItems(instances)) ids.insert(item.stable_identity.instance);
            REQUIRE(ids.size() == 1);
            cbPose(*ids.begin(), original);
            const auto primitive_materials = instances.instancePrimitiveMaterials(*ids.begin());
            REQUIRE(primitive_materials.size() == 2);
            std::size_t painted_count = 0;
            for (const auto &item : primitive_materials) painted_count += item.material == paint;
            REQUIRE(painted_count == (std::string_view{scene} == "combined" ? (row == 4 ? 2 : 1) : 0));
            return cbRead(row);
        };
        a = sceneCapture("baseline");
        b = sceneCapture("combined");
    } else if (row == 10) {
        auto second = painted;
        const auto other = GET_MODULE(ProjectMaterialAssetContainer).materialByName("SecondPaint");
        for (auto &group : second.material_primitives) group.material = other;
        const auto left = capture(painted, true, -0.7F), right = capture(second, true, 0.7F);
        a["main"] = TestSupport::unionCoverage(left.at("main"), right.at("main"));
        const auto first = instances.placeModelInstance(painted), last = instances.placeModelInstance(second);
        cbPose(first, painted); cbPose(last, second);
        instances.setTrs(first, {-0.7F,0,0}, glm::quat{1,0,0,0}, {1,1,1});
        instances.setTrs(last, {0.7F,0,0}, glm::quat{1,0,0,0}, {1,1,1});
        b = cbRead(row);
        REQUIRE(instances.removeModelInstance(first)); REQUIRE(instances.removeModelInstance(last));
    } else if (row == 9) {
        const auto id = instances.placeModelInstance(painted);
        cbPose(id, painted);
        a = cbRead(row);
        auto &materials = GET_MODULE(MaterialContainer);
        auto &pipelines = GET_MODULE(PipelineFactory);
        const auto pipeline = materials.materialPipeline(paint, VertexMode::skinned);
        const auto before = pipelines.pipeline(pipeline);
        cbWrite(fixture.root / "paint.surface", cbSurface +
            "\nvoid pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) { vertex.position.z += 0.00001; }\n");
        REQUIRE(GET_MODULE(watch::ReloadService).applyRequestForTesting({.key = watch::makeAssetKey("paint.surface")}));
        CHECK(pipelines.pipeline(pipeline) != before);
        REQUIRE(instances.isModelInstanceAlive(id));
        b = cbRead(row);
        REQUIRE(instances.removeModelInstance(id));
    } else {
        a = capture(row == 8 ? painted : original);
        if (row == 8) {
            auto &materials = GET_MODULE(MaterialContainer);
            auto &pipelines = GET_MODULE(PipelineFactory);
            const auto pipeline = materials.materialPipeline(paint, VertexMode::standard);
            const auto before = pipelines.pipeline(pipeline);
            cbWrite(fixture.root / "paint.surface", cbSurface +
                "\nvoid pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) { vertex.position.x += 0.7; }\n");
            REQUIRE(GET_MODULE(watch::ReloadService).applyRequestForTesting({.key = watch::makeAssetKey("paint.surface")}));
            CHECK(pipelines.pipeline(pipeline) != before);
        }
        if (row == 3) {
            const auto glass = GET_MODULE(ProjectMaterialAssetContainer).materialByName("Glass");
            for (auto &group : painted.material_primitives) group.material = glass;
        }
        b = capture(painted);
        if (row == 2) {
            REQUIRE(std::any_of(records.begin(), records.end(), [&](const auto &record) {
                if (record.kind != "material" || record.material != paint.value || record.effective_material == paint.value) return false;
                const auto &desc = GET_MODULE(PipelineFactory).graphicsDesc(PipelineHandle{record.pipeline});
                CHECK(desc.use_skinned_vertex_layout);
                CHECK(desc.cull_mode == vk::CullModeFlagBits::eFront);
                return true;
            }));
            const auto image = RendererTestAccess::readRenderTarget(renderer, "lit_color", {});
            REQUIRE(image.format == vk::Format::eR16G16B16A16Sfloat);
            std::size_t red = 0;
            for (std::size_t i = 0; i < image.bytes.size(); i += 8) {
                std::array<std::uint16_t, 4> rgba;
                std::memcpy(rgba.data(), image.bytes.data() + i, 8);
                red += rgba[0] == 0x3c00 && rgba[1] == 0 && rgba[2] == 0;
            }
            CHECK(red > 16);
        }
        if (row == 1 || row == 6 || row == 7) {
            const auto neutral = capture(original, false);
            CHECK(TestSupport::compareCoverage(a.at("main"), neutral.at("main")).different > 16);
        }
    }
    if (row == 8) {
        CHECK(TestSupport::compareCoverage(a.at("main"), b.at("main")).different > 16);
        fixture.compare(a.at("main"), a.at("shadow"), "no_hook_shadow_control");
        fixture.compare(a.at("main"), a.at("velocity"), "no_hook_velocity_control");
        fixture.compare(a.at("main"), a.at("picking"), "no_hook_picking_control");
        Json mismatches = Json::array();
        for (const auto *pass : {"shadow", "velocity", "picking"}) {
            if (!fixture.compare(b.at("main"), b.at(pass), pass, true)) {
                // Only the measured limitation (auxiliary pass stays at the
                // undisplaced geometry) is covered by CB-1/08.
                REQUIRE(TestSupport::compareCoverage(a.at(pass), b.at(pass)).equivalent());
                mismatches.push_back(pass);
            }
        }
        if (!mismatches.empty()) cbKnownRed(fixture,
            "CB-1/08 vertex_displace_v1 absent from auxiliary passes (VD-1)", mismatches);
    } else if (row == 3) {
        // Blend deliberately leaves depth untouched. Compare rendered color
        // support and independently verify both routes' depth contracts.
        fixture.compare(a.at("color_support"), b.at("color_support"), "main");
        fixture.compare(a.at("main"), a.at("color_support"), "opaque_depth_control");
        CHECK(b.at("main").count() == 0);
    } else {
        for (const auto &[pass, mask] : a) fixture.compare(mask, b.at(pass), pass);
    }
}

void runCombination(int row) {
    setupLogger();
    CombinationFixture fixture{row};
    std::filesystem::remove(fixture.root / "known_red.json");
    runCombination(fixture);

}
#endif
} // namespace

#if PELICAN_RUNTIME_SHADER_COMPILER
TEST_CASE("CB1 01 posed skin material bindings main and shadow", "[headless][gpu][cb1]") { runCombination(1); }
TEST_CASE("CB1 02 skin outline variant and when skinned draw loop", "[headless][gpu][cb1]") { runCombination(2); }
TEST_CASE("CB1 03 skin forward blend material", "[headless][gpu][cb1]") { runCombination(3); }
TEST_CASE("CB1 04 skin scene material override all primitives", "[headless][gpu][cb1]") { runCombination(4); }
TEST_CASE("CB1 05 skin scene material override one primitive", "[headless][gpu][cb1]") { runCombination(5); }
TEST_CASE("CB1 06 weighted morph without skin main and shadow", "[headless][gpu][cb1]") { runCombination(6); }
TEST_CASE("CB1 07 VRM skin and happy expression", "[headless][gpu][cb1]") { runCombination(7); }
TEST_CASE("CB1 08 vertex displacement main shadow velocity picking", "[headless][gpu][cb1]") { runCombination(8); }
TEST_CASE("CB1 09 posed skin surface hot reload", "[headless][gpu][cb1]") { runCombination(9); }
TEST_CASE("CB1 10 two skin instances with different materials", "[headless][gpu][cb1]") { runCombination(10); }
TEST_CASE("CB1 11 static project material ray query shadow", "[headless][gpu][cb1]") { runCombination(11); }
TEST_CASE("CB1 12 static project save and reopen", "[headless][gpu][cb1]") { runCombination(12); }
#else
TEST_CASE("CB1 combinations require runtime shader compiler", "[headless][gpu][cb1]") { SKIP("runtime shader compiler unavailable"); }
#endif
} // namespace Pelican
