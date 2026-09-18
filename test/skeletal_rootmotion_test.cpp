#include "../src/core/animation/animationservice.hpp"
#include "../src/core/animation/posegraphruntime.hpp"
#include "../src/core/communication/animationstatus.hpp"
#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/ecs/core.hpp"
#include "../src/core/ecs/predefined.hpp"
#include "../src/core/ecs/predefined/animationsystem.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/loader/basicconfig.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/loader/scene.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/polygoninstancecontainer.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/userpublic/components/localtransform.hpp"
#include "../src/core/userpublic/components/modelview.hpp"
#include "../src/core/userpublic/gameobjects.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"
#include "../src/project/graphcomposition.hpp"
#include "graph_composition_fixture.hpp"
#include "root_motion_fixture.hpp"
#include "scene_load_test_support.hpp"
#include "vulkan_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace Pelican {
namespace {

void writeText(const std::filesystem::path &path, std::string_view value) {
    std::ofstream file{path, std::ios::binary};
    file << value;
}

struct TempDir {
    std::filesystem::path path;
    ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

// AG-2 acceptance 1-3. The clip's root travels two metres per loop; the
// individual travels with it and the mesh stops sliding inside the instance.
// Everything observed here came through the production path: scene load, the
// ECS update, the animation service's phase run, and -- for the pose itself --
// the GPU instance attribute the compute dispatch reads back.
//
// Generated rather than sectioned, so no case can end up asserting nothing.
TEST_CASE("a pose graph's root motion moves the individual",
          "[skeletal][root-motion][graph-pose-root]") {
    setupLogger();
    const auto mode = GENERATE(as<std::string>{}, "extract", "none", "wrap", "reverse", "fast",
                               "seek", "seamless loop", "no local transform",
                               "no translation channel", "no duration",
                               "the individual goes away after the phase was prepared");
    CAPTURE(mode);
    const bool extract = mode != "none";
    // C-5. The one failure that happens *inside* the phase callback rather than
    // at attach: everything the document could decide was decided at bind, so
    // what is left is the individual disappearing between the owner thread's
    // synchronize and the phase that writes it.
    const bool fails_in_phase = mode == "the individual goes away after the phase was prepared";
    const bool expects_rejection = mode == "no local transform" ||
                                   mode == "no translation channel" || mode == "no duration";

    const auto root = std::filesystem::temp_directory_path() /
        ("pelican_root_motion_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "passes");
    std::filesystem::create_directories(root / "shaders");
    const TempDir guard{root};
    TestRootMotionFixture::writeGltf(root / "mover.gltf", "mover.bin",
        TestRootMotionFixture::Options{.seamless_loop = mode == "seamless loop",
                                       .single_key = mode == "no duration"});

    nlohmann::json sampler{{"id", "source"}, {"op", "pose.clip_sample"},
        {"parameters", {{"rig", "project://mover.gltf"},
                        {"clip", mode == "no translation channel" ? "Turn" : "Advance"}}},
        {"inputs", {{"time", {{"node", "time"}, {"port", "value"}}}}},
        {"outputs", {"pose"}}};
    if (extract) {
        sampler["parameters"]["root_motion"] = "extract";
        sampler["parameters"]["root_node"] = 0;
    }
    if (mode == "reverse") sampler["parameters"]["speed"] = -1.0;
    if (mode == "fast") sampler["parameters"]["speed"] = 2.5;

    auto document = parameterGraphCompositionFixture();
    auto nodes = nlohmann::json::parse(R"([
      {"id":"time","op":"graph.input"},
      {"id":"model","op":"render.attribute","parameters":{"element":"mat4","domain":"instance",
        "source":{"asset":"project://mover.gltf","pose_node":0}},"outputs":["resource"]},
      {"id":"out","op":"pose.skeletal","inputs":{"pose":{"node":"source","port":"pose"}},
       "outputs":[]}])");
    nodes.push_back(std::move(sampler));
    document["definitions"]["pose"] = {{"nodes", std::move(nodes)},
        {"outputs", {{"model", {{"node", "model"}, {"port", "resource"}}}}}};
    constexpr int instance_index = 5;
    document["nodes"][instance_index] = {{"id", "render"}, {"op", "graph.instance"},
        {"definition", "pose"}, {"owner", {{"scene", "default_scene"}, {"object", 1}}},
        {"inputs", {{"time", {{"node", "clock"}, {"port", "value"}}}}}};
    document["nodes"][instance_index + 1]["parameters"]["size"] = 16;
    auto &probe = document["nodes"][instance_index + 2];
    probe["parameters"]["ports"].erase("source");
    probe["inputs"].erase("source");
    probe["parameters"]["ports"]["model"] =
        {{"element", "mat4"}, {"domain", "instance"}, {"access", "read"}};
    probe["inputs"]["model"] = {{"node", "render"}, {"port", "model"}};
    document["rendering"] = {{"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}}};
    writeText(root / "passes/main_rendering_config.json", document.dump());
    // The pose the graph published, read back off the GPU: the model matrix of
    // the very node the document called the root. With the root pinned this is
    // the clip's first key however far the individual has travelled; without
    // the extraction it is the clip's own slide.
    writeText(root / "shaders/graph_probe.comp", R"(
#version 450
#extension GL_GOOGLE_include_directive : enable
#include "pelican_resource_ports.glsl"
layout(local_size_x=1,local_size_y=1,local_size_z=1) in;
void main() {
    mat4 model = pelican_load_model(0);
    pelican_store_destination(0, model[3].x);
    pelican_store_destination(1, model[3].y);
    pelican_store_destination(2, model[3].z);
    pelican_store_destination(3, model[0].x);
}
)");
    writeText(root / "assets.json",
        R"({"schema":"pelican.asset_data","version":1,"models":[{"name":"mover","path":"mover.gltf"}]})");
    // No animation component: the graph is the only thing that can pose this
    // object or move it. `localtransform` is declared by hand -- the loader adds
    // one only to an object that takes part in a parent/child scene, and a
    // parented individual would see its world transform recomposed a frame
    // later, so the shape that is correct on the same frame is this one.
    auto scene_document = nlohmann::json::parse(R"({"schema":"pelican.scene","version":1,
      "scenes":{"default_scene":{"objects":[{"name":"Mover","components":[
        {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"localtransform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
        {"name":"simplemodelview","model":"mover"}]}]}}})");
    if (mode == "no local transform")
        scene_document["scenes"]["default_scene"]["objects"][0]["components"].erase(1);
    writeText(root / "scene.json", scene_document.dump());

    auto &service = Animation::animationServiceRuntime();
    service.reset();
    Animation::poseGraphRuntime().clear();
    FastModuleContainer modules;
    GET_MODULE(PathResolver).setup(root, false);
    GET_MODULE(ProjectSource).setSourceByData(nlohmann::json{{"basic_config", {
        {"scene_data_json", "scene.json"}, {"asset_data_json", "assets.json"}}}}.dump());
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    TestSupport::requireVulkanDevice("Vulkan unavailable");
    GET_MODULE(ECSPredefinedRegistration).reg();
    auto &scene = GET_MODULE(SceneLoader);
    test_support::loadScene(scene, "default_scene");
    auto &ecs = GET_MODULE(ECSCore).getTemplatePublicModule();
    auto &time = GET_MODULE(EngineTime);
    constexpr double step = 1.0 / 60.0;
    time.setup(EngineTime::Mode::fixed_step, step);
    auto &instances = GET_MODULE(PolygonInstanceContainer);
    const auto object = scene.objectId("Mover");
    REQUIRE(object);
    for (const auto &binding : scene.runtimeObjectBindings())
        if (binding.object_id == *object)
            document["nodes"][instance_index]["owner"]["object"] = binding.authoring_object_id.value;
    writeText(root / "passes/main_rendering_config.json", document.dump());

    const auto animate = [&] {
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::ok);
        scene.applyPendingLoad();
    };
    auto &renderer = GET_MODULE(Renderer);
    auto &core = GET_MODULE(VulkanManageCore);
    const auto render = [&] {
        renderer.render();
        core.waitIdle();
        const auto bytes = core.readBuf(
            GET_MODULE(FrameGraphResourceContainer).buffer("result"), sizeof(float) * 4);
        std::array<float, 4> values;
        std::memcpy(values.data(), bytes.data(), sizeof(values));
        return values;
    };
    const auto *view = ecs.tryComponent<SimpleModelViewComponent>(*object);
    REQUIRE(view);
    REQUIRE(view->model_instance_id);
    // The pass document is on disk before the renderer is built, and the owner
    // object id is patched into the published copy. The published program
    // reaches the animation tick only once the renderer has taken it, so every
    // warm-up below draws as well as ticks. Nothing is measured until the
    // graph is attached.
    const auto publish = [&] {
        GET_MODULE(ProjectBasicConfig).publishRenderingConfigJson(document.dump());
    };

    if (expects_rejection) {
        // Nothing here is decidable from the document alone: whether the object
        // can take a local transform, whether the clip translates the node the
        // document called the root, whether that clip has a duration at all.
        // All three are named when the graph attaches rather than silently
        // turning the extraction into a no-op.
        std::string code;
        for (int frame = 0; frame < 8 && code.empty(); ++frame) {
            try {
                animate();
                renderer.render();
                core.waitIdle();
            } catch (const GraphRegistryError &error) {
                code = error.code();
            }
            if (frame == 0) publish();
        }
        if (mode == "no local transform") REQUIRE(code == "pose.root_motion_no_transform@1");
        else REQUIRE(code == "pose.root_motion_unsupported@1");
        service.reset();
        Animation::poseGraphRuntime().clear();
        return;
    }

    animate();
    publish();
    bool attached = false;
    for (int frame = 0; frame < 8 && !attached; ++frame) {
        animate();
        (void)render();
        attached = !Animation::poseGraphRuntime().trace().empty();
    }
    REQUIRE(attached);

    if (fails_in_phase) {
        // The frame is prepared exactly as `animate` prepares one, and then the
        // individual the sink was about to move is removed -- after the owner
        // thread is done, while only the phase is left to run. Applying the
        // root displacement fails there, inside a C callback an exception
        // cannot cross.
        time.advance();
        GET_MODULE(ECSCore).update();
        service.synchronizeSceneObjects();
        GET_MODULE(AnimationSystem).updateGraphs();
        REQUIRE(GameObjects::remove(*object));
        REQUIRE(service.runAllPhases(time.frameIndex()) == Animation::Status::callback_failed);
        std::string message;
        try {
            // What the frame loop does with a non-ok phase result: ask the pose
            // graph for the exception the callback had to swallow.
            Animation::poseGraphRuntime().rethrowPendingFailure();
        } catch (const std::exception &error) {
            message = error.what();
        }
        CAPTURE(message);
        // The object is still named -- that much was always there.
        REQUIRE_THAT(message,
            Catch::Matchers::ContainsSubstring("pose graph evaluation failed for Mover"));
        // And this is what C-5 adds: the evaluation's own words survive the
        // callback boundary. Before it, `what()` stopped at the object name and
        // the author was left to guess which node of which graph refused.
        REQUIRE_THAT(message,
            Catch::Matchers::ContainsSubstring("pose.root_motion_no_transform@1"));
        REQUIRE_THAT(message, Catch::Matchers::ContainsSubstring("render/out"));
        service.reset();
        Animation::poseGraphRuntime().clear();
        return;
    }

    // The clock and the individual both start from zero, so every number below
    // is the one the ledger names rather than an offset from whichever frame
    // the published program happened to attach on. setTime is also the first
    // half of acceptance 3: the frame that observes it contributes nothing,
    // which is what leaves the individual exactly where it was.
    const auto travelled = [&] { return GameObjects::localTransform(*object).pos.x; };
    time.setTime(-step);
    auto origin = GameObjects::localTransform(*object);
    origin.pos = vec3{0.0f, 0.0f, 0.0f};
    REQUIRE(GameObjects::setLocalTransform(*object, origin));
    animate();
    // CHECK, not REQUIRE: this is the same "a seek is not a movement" rule the
    // seek case measures, and a build that gets it wrong should be allowed to
    // reach that case and report the number there too.
    CHECK(std::abs(travelled()) < 1e-6f);
    REQUIRE(std::abs(time.now()) < 1e-9);

    const auto step_frames = [&](int count) { for (int i = 0; i < count; ++i) animate(); };

    if (mode == "extract" || mode == "none") {
        // Acceptance 1 and its control. Half the clip in thirty frames at
        // 1/60: the root is halfway along its two metres.
        step_frames(30);
        const auto values = render();
        // The pose the GPU drew, and the matrix the renderer holds for the same
        // node, agree -- so the pin is observed on the frame that was drawn.
        const auto matrix = instances.animationNodeMatrix(*view->model_instance_id, 0);
        REQUIRE(matrix);
        REQUIRE(std::abs(values[0] - (*matrix)[3].x) < 0.00001f);
        if (extract) {
            REQUIRE(std::abs(travelled() - 1.0f) < 0.0001f);
            REQUIRE(std::abs(values[0]) < 0.0001f);
        } else {
            REQUIRE(std::abs(travelled()) < 0.0001f);
            REQUIRE(std::abs(values[0] - 1.0f) < 0.0001f);
        }
    } else if (mode == "wrap") {
        // Acceptance 2. Ninety frames crosses the end of the clip: the
        // individual keeps going instead of snapping back to where the clip
        // starts, because a whole loop of the clip is a whole loop of travel.
        step_frames(90);
        (void)render();
        REQUIRE(std::abs(travelled() - 3.0f) < 0.001f);
    } else if (mode == "reverse") {
        // The same arithmetic run backwards.
        step_frames(30);
        (void)render();
        REQUIRE(std::abs(travelled() + 1.0f) < 0.001f);
    } else if (mode == "fast") {
        // Two and a half loops in sixty frames: more than one wrap inside the
        // measured window, and the total is still the distance the root covered.
        step_frames(60);
        (void)render();
        REQUIRE(std::abs(travelled() - 5.0f) < 0.001f);
    } else if (mode == "seamless loop") {
        // The clip's own convention decides how far a loop travels. This one is
        // authored with its last key equal to its first, so a whole loop is
        // worth nothing and the individual returns to where it started. Two
        // whole loops in 120 frames.
        step_frames(120);
        (void)render();
        REQUIRE(std::abs(travelled()) < 0.001f);
    } else if (mode == "seek") {
        // Acceptance 3. Thirty frames of travel, then the clock is set back.
        step_frames(30);
        REQUIRE(std::abs(travelled() - 1.0f) < 0.0001f);
        time.setTime(0.25);
        animate();
        // A seek is a cut, not a movement: the individual does not jump by the
        // distance between the two times (which would be about -0.47 here).
        REQUIRE(std::abs(travelled() - 1.0f) < 2.0f / 60.0f);
        // And it starts moving again from there. An implementation that keeps
        // the set_time bookkeeping where only a state machine can reach it
        // would look identical on the frame above and then never move again.
        step_frames(30);
        (void)render();
        REQUIRE(std::abs(travelled() - 2.0f) < 0.001f);
    } else {
        FAIL("unhandled mode");
    }

    // One evaluation per frame revision, and the sink still names the node that
    // produced the pose: extracting root motion did not change what the trace
    // records.
    const auto trace = Animation::poseGraphRuntime().trace();
    REQUIRE(trace.size() == 1);
    REQUIRE(trace.front().sink == "render/out");
    REQUIRE(trace.front().node == "render/source");
    REQUIRE(trace.front().frame_revision == time.frameIndex());
    // AG-3a acceptance 1. What this frame handed the individual is observable,
    // and only on a chain that asks for it. A document that does not extract
    // reports no displacement rather than a zero that could mean either.
    REQUIRE(trace.front().root_motion == extract);
    if (mode == "extract") {
        // The clip carries its root two metres in its one second, so one frame
        // of it is two metres times this frame's step -- the amount applied,
        // not the metre travelled since the clock started.
        REQUIRE(std::abs(trace.front().root_delta[0] - 2.0 * step) < 1e-6);
    }
    if (!extract) REQUIRE(std::abs(trace.front().root_delta[0]) < 1e-12);
    // The chain is one clip_sample, so the sink says so and names the clip it
    // stands on.
    REQUIRE(std::string{Animation::PoseGraphRuntime::sinkSourceName(trace.front().source)} ==
            "pose_graph_clip");
    REQUIRE(trace.front().cursors.size() == 1);
    REQUIRE(trace.front().cursors.front().clip == "Advance");
    REQUIRE(trace.front().owner.scene == "default_scene");
    REQUIRE(trace.front().owner.object != 0);
    // And the same numbers through the section get_status carries. root_delta
    // is present exactly when the chain extracts one, so a tool cannot read
    // "no extraction" and "a standing individual" as the same answer.
    const auto status = internal::animationStatusJson();
    REQUIRE(status.at("available") == true);
    REQUIRE(status.at("sinks").size() == 1);
    const auto &reported = status.at("sinks").at(0);
    REQUIRE(reported.contains("root_delta") == extract);
    if (mode == "extract")
        REQUIRE(std::abs(reported.at("root_delta").at(0).get<double>() - 2.0 * step) < 1e-6);
    service.reset();
    Animation::poseGraphRuntime().clear();
}

} // namespace Pelican
