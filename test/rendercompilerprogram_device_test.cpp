// RC-3c1b: the production compiler program on a device (a ray query needs
// the device's): the final references of ray_task_acceleration_structures,
// in the runtime variants and in the preview; and the registration's own
// way to the compiler (its requests, its providers, its launch
// configuration).
#include "rendercompilerprogram_ir_test_support.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/frameresources.hpp"
#include "../src/core/renderer/shadowdepthpasscontainer.hpp"
#include "../src/core/renderer/velocitypasscontainer.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/computetaskdefinition.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/core/vkcore/runtimecapabilities.hpp"
#include "../src/project/frameplan.hpp"
#include "../src/project/rayrecordabi.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <map>

namespace Pelican {
namespace {

// The first physical device, planned with the ray capabilities.
struct Device {
    vk::UniqueInstance instance;
    vk::PhysicalDevice physical;
    VulkanRuntimeCapabilities capabilities;
    std::optional<VulkanRenderCompilerBackendContext> backend;

    Device() {
        const vk::ApplicationInfo application{"rc3c1b-device", 1, "pelican", 1, VK_API_VERSION_1_3};
        instance = vk::createInstanceUnique(vk::InstanceCreateInfo{{}, &application});
        const auto devices = instance->enumeratePhysicalDevices();
        REQUIRE_FALSE(devices.empty());
        physical = devices.front();
        capabilities.multiview = true;
        capabilities.ray_query = true;
        capabilities.ray_tracing_pipeline = true;
        backend.emplace(vk::Format::eB8G8R8A8Srgb, vk::Extent2D{64, 64}, physical,
                        std::vector<std::string>{}, capabilities,
                        VulkanRenderCompilerDevicePlanningMode::device_required);
    }
};

Json rayQueryTask(const std::string &name, const std::string &structure) {
    return Json{{"name", name},
                {"shader", "engine://shaders/compute/fixture"},
                {"writes", Json::array({"hits"})},
                {"dispatch", {{"groups", Json::array({1, 1, 1})}}},
                {"ray_query", {{"acceleration_structure", structure}}}};
}

// The engine's registration dependencies (the headless renderer's modules).
RenderingPassConfigRegistrationDependencies
registrationDependencies(RenderingPassConfigRegistrationDependencies::Options options) {
    return {
        {GET_MODULE(RenderTargetContainer)},
        {
            GET_MODULE(RenderTarget),
            GET_MODULE(ShaderLibrary),
            GET_MODULE(FullscreenPassContainer),
            GET_MODULE(PipelineFactory),
            GET_MODULE(ShadowDepthPassContainer),
            GET_MODULE(VelocityPassContainer),
            GET_MODULE(PathResolver),
            {},
            true,
            []() -> DebugDraw & { return GET_MODULE(DebugDraw); },
            {},
            []() -> DebugText & { return GET_MODULE(DebugText); },
        },
        GET_MODULE(FrameGraphResourceContainer),
        GET_MODULE(ComputeTaskContainer),
        GET_MODULE(FrameGraphRuntimeContainer),
        GET_MODULE(RenderingPassContainer),
        std::move(options),
    };
}

// What the registration handed the production program and what it made,
// recorded; then the registration stops (the GPU objects of an ImGui pass
// need the ImGui context, which this test does not start).
struct Compiled {};
struct RecordingProgram final : RenderCompilerProgram {
    mutable std::vector<RenderPipelineGraphVariant> requested;
    mutable bool flat_composes = false;
    mutable std::size_t parses = 0;
    mutable bool imgui_in_ir = false;
    mutable bool imgui_in_plan = false;
    mutable std::vector<std::string> flat_order;
    mutable bool preview_available = false;

    RenderCompilerProgramSelection selection(const RenderCompilerBackendContext &backend) const override {
        return defaultVulkanRenderCompilerProgram().selection(backend);
    }
    RenderCompilerProgramOutput compile(const RenderCompilerProgramInput &input) const override {
        for (const auto &request : input.variants) {
            requested.push_back(request.graph_variant);
            if (request.graph_variant == RenderPipelineGraphVariant::flat)
                flat_composes = static_cast<bool>(request.compose_runtime_config);
        }
        const auto before = renderGraphParseCount();
        auto output = defaultVulkanRenderCompilerProgram().compile(input);
        parses = renderGraphParseCount() - before;
        for (const auto &variant : output.variants) {
            if (variant.graph_variant == RenderPipelineGraphVariant::preview) {
                preview_available = !variant.unavailable;
                continue;
            }
            if (variant.graph_variant != RenderPipelineGraphVariant::flat) continue;
            if (variant.unavailable) std::rethrow_exception(variant.unavailable);
            for (const auto &node : variant.render_graph->graphs.at(0).nodes)
                imgui_in_ir = imgui_in_ir || (node.name == "imgui_pass" && node.shape == RenderNodeShape::imgui);
            flat_order = framePlanOrder(variant.frame_plans.at("main"));
            imgui_in_plan = std::find(flat_order.begin(), flat_order.end(), "imgui_pass") != flat_order.end();
        }
        throw Compiled{};
    }
};

// The XR variant's physical plan as the registration's compile made it:
// whether its graph runs its views with multiview and whether it exports a
// depth image, or what refused it.
struct XrRecordingProgram final : RenderCompilerProgram {
    mutable std::vector<std::pair<bool, bool>> requested_xr_flags;
    mutable std::optional<bool> uses_multiview;
    mutable std::optional<std::string> exported_depth;
    mutable std::string refusal;

    RenderCompilerProgramSelection selection(const RenderCompilerBackendContext &backend) const override {
        return defaultVulkanRenderCompilerProgram().selection(backend);
    }
    RenderCompilerProgramOutput compile(const RenderCompilerProgramInput &input) const override {
        for (const auto &request : input.variants)
            if (request.graph_variant == RenderPipelineGraphVariant::xr)
                requested_xr_flags.emplace_back(request.enable_multiview_runtime,
                                                request.enable_external_depth_export);
        try {
            const auto output = defaultVulkanRenderCompilerProgram().compile(input);
            for (const auto &variant : output.variants) {
                if (variant.graph_variant != RenderPipelineGraphVariant::xr) continue;
                const auto &physical = requireVulkanRenderCompilerPhysicalPackage(*variant.physical_package);
                REQUIRE(physical.target_plan_compilation.plans.size() == 1);
                const auto &plan = *physical.target_plan_compilation.plans.front();
                uses_multiview = plan.view_execution_plan.uses_multiview;
                exported_depth = plan.external_depth_export ? plan.external_depth_export->source_resource
                                                            : std::string{};
            }
        } catch (const std::exception &error) {
            refusal = error.what();
        }
        throw Compiled{};
    }
};

} // namespace

TEST_CASE("RC-3c1b the registration carries the XR variant's multiview and depth export to its physical plan",
          "[rc3c1b][registration][xr][gpu]") {
#if PELICAN_WITH_OPENXR
    // A flat and an XR variant registered together; the XR variant's
    // options say whether multiview runs and whether depth is exported. Each
    // flag changes the XR variant's physical plan, the other kept off.
    // Multiview: the graph's passes are builtin fullscreen ones and its
    // automatic policy has a measured gain on this device (a profile with
    // the device's vendor and device id); with the flag the plan runs views
    // with multiview, without it no pass may and every view runs in turn.
    // Depth export: `draw` (a material pass) also writes a depth image; with
    // the flag the plan exports it, without it nothing.
    setupLogger();
    struct Outcome {
        std::vector<std::pair<bool, bool>> requested;
        std::optional<bool> uses_multiview;
        std::optional<std::string> exported_depth;
        std::string refusal;
    };
    const auto registered = [](Json rendering, bool multiview, bool depth_export) {
        FastModuleContainer modules;
        Project project;
        std::ofstream{project.root / "scene.json", std::ios::binary}
            << R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json";
        std::ofstream{project.root / "assets.json", std::ios::binary}
            << R"json({"schema":"pelican.asset_data","version":2,"models":[]})json";
        GET_MODULE(ProjectSource)
            .setSourceByData(Json{{"basic_config",
                                   {{"window_size", {{"width", 16}, {"height", 16}}},
                                    {"scene_data_json", "scene.json"},
                                    {"asset_data_json", "assets.json"},
                                    {"default_scene_id", "default_scene"}}}}
                                 .dump());
        GET_MODULE(PathResolver).setup(project.root, false);
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{16, 16};
        if (rendering.contains("xr")) {
            // This device's profile (the device exists once the target does).
            (void)GET_MODULE(RenderTarget).getExtent();
            const auto properties = GET_MODULE(VulkanManageCore).getPhysDevice().getProperties();
            rendering["xr"]["multiview_auto"]["profiles"][0]["vendor_id"] = properties.vendorID;
            rendering["xr"]["multiview_auto"]["profiles"][0]["device_id"] = properties.deviceID;
        }
        XrRecordingProgram program;
        std::vector<RenderingPassConfigRegistrationDependencies> runtime;
        {
            // As the renderer registers an XR launch's pair: one owner
            // scope, one feature publisher.
            RenderingPassConfigRegistrationDependencies::Options flat;
            flat.render_compiler_program = &program;
            flat.gpu_owner_scope = renderPipelineVariantGpuOwnerScope(true, RenderPipelineGraphVariant::flat);
            runtime.push_back(registrationDependencies(std::move(flat)));
            RenderingPassConfigRegistrationDependencies::Options xr;
            xr.graph_variant = RenderPipelineGraphVariant::xr;
            xr.enable_multiview_runtime = multiview;
            xr.enable_external_depth_export = depth_export;
            xr.publish_enabled_features = false;
            xr.gpu_owner_scope = renderPipelineVariantGpuOwnerScope(true, RenderPipelineGraphVariant::xr);
            xr.render_compiler_program = &program;
            runtime.push_back(registrationDependencies(std::move(xr)));
        }
        const auto document = project.rootOf(rendering).dump();
        CHECK_THROWS_AS(registerRenderGraphVariantFamilyFromJsonData(document, {16, 16}, std::move(runtime)),
                        Compiled);
        return Outcome{program.requested_xr_flags, program.uses_multiview, program.exported_depth,
                       program.refusal};
    };

    auto multiview = section(0, false);
    multiview["xr"] = {
        {"view_execution", "auto"},
        {"multiview_auto",
         {{"profiles", Json::array({Json{{"id", "rc3c1b_this_device"},
                                         {"vendor_id", 0},
                                         {"measurement",
                                          {{"sequential_gpu_ms", 6.0},
                                           {"multiview_gpu_ms", 4.0},
                                           {"sample_count", 240},
                                           {"source", "RenderTiming/gpu_timestamp"}}}}})}}}};
    {
        const auto on = registered(multiview, true, false);
        INFO(on.refusal);
        CHECK(on.requested == std::vector<std::pair<bool, bool>>{{true, false}});
        CHECK(on.uses_multiview == std::optional<bool>{true});
        CHECK(on.exported_depth == std::optional<std::string>{""});
        const auto off = registered(multiview, false, false);
        INFO(off.refusal);
        CHECK(off.requested == std::vector<std::pair<bool, bool>>{{false, false}});
        CHECK(off.uses_multiview == std::optional<bool>{false});
    }

    auto depth = section(0, false);
    depth["render_targets"].push_back(
        Json{{"name", "depth"}, {"extent_scale", 1.0}, {"format", "D32_SFLOAT"},
             {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}});
    depth["rendering_passes"][0]["passes"][0] =
        Json{{"name", "draw"},
             {"type", "material"},
             {"output", {{"color", Json::array({"scene"})}, {"depth", "depth"}}}};
    {
        const auto on = registered(depth, false, true);
        INFO(on.refusal);
        CHECK(on.requested == std::vector<std::pair<bool, bool>>{{false, true}});
        CHECK(on.exported_depth == std::optional<std::string>{"depth"});
        const auto off = registered(depth, false, false);
        INFO(off.refusal);
        CHECK(off.requested == std::vector<std::pair<bool, bool>>{{false, false}});
        CHECK(off.exported_depth == std::optional<std::string>{""});
        CHECK(on.uses_multiview == std::optional<bool>{false});
        CHECK(off.uses_multiview == std::optional<bool>{false});
    }
#else
    SKIP("an XR variant needs PELICAN_WITH_OPENXR");
#endif
}

TEST_CASE("RC-3c1b the registration composes ImGui into the flat IR and FramePlan as the launch configuration says",
          "[rc3c1b][registration][imgui][gpu]") {
    // registerRenderGraphVariantFamilyFromJsonData with a graph transform and
    // a subgraph replacement in the engine's registries. The headless
    // renderer is made first; the launch configuration then says whether
    // ImGui runs. The flat request carries the ImGui composition either way;
    // on, the final IR and the FramePlan have imgui_pass; off, neither has
    // it. Each revision is parsed once (flat: input, transform, replacement;
    // the preview once).
    setupLogger();
    const auto registered = [](bool imgui) {
        FastModuleContainer modules;
        Project project;
        std::ofstream{project.root / "scene.json", std::ios::binary}
            << R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json";
        std::ofstream{project.root / "assets.json", std::ios::binary}
            << R"json({"schema":"pelican.asset_data","version":2,"models":[]})json";
        GET_MODULE(ProjectSource)
            .setSourceByData(Json{{"basic_config",
                                   {{"window_size", {{"width", 16}, {"height", 16}}},
                                    {"scene_data_json", "scene.json"},
                                    {"asset_data_json", "assets.json"},
                                    {"default_scene_id", "default_scene"}}}}
                                 .dump());
        GET_MODULE(PathResolver).setup(project.root, false);
        auto &launch = GET_MODULE(EngineLaunchConfig);
        launch.headless = true;
        launch.headless_extent = vk::Extent2D{16, 16};
        Providers providers{true};
        RecordingProgram program;
        RenderingPassConfigRegistrationDependencies::Options options;
        options.render_compiler_program = &program;
        std::vector<RenderingPassConfigRegistrationDependencies> runtime;
        runtime.push_back(registrationDependencies(std::move(options)));
        launch.headless = !imgui;
        const auto document = project.rootOf(section(1, true)).dump();
        CHECK_THROWS_AS(registerRenderGraphVariantFamilyFromJsonData(document, {16, 16}, std::move(runtime)),
                        Compiled);
        INFO("flat order: " << Json(program.flat_order).dump());
        CHECK(program.requested == std::vector{RenderPipelineGraphVariant::flat, RenderPipelineGraphVariant::preview});
        CHECK(program.preview_available);
        CHECK(std::find(program.flat_order.begin(), program.flat_order.end(), "t1_pass") != program.flat_order.end());
        return std::tuple{program.parses, program.flat_composes, program.imgui_in_ir, program.imgui_in_plan};
    };
#if PELICAN_WITH_IMGUI
    CHECK(registered(true) == std::tuple{std::size_t{1 + 1 + 1 + 1}, true, true, true});
    CHECK(registered(false) == std::tuple{std::size_t{1 + 1 + 1 + 1}, true, false, false});
#else
    CHECK(registered(true) == std::tuple{std::size_t{1 + 1 + 1 + 1}, false, false, false});
#endif
}

TEST_CASE("RC-3c1b the table selects the declared structure for a task a provider adds",
          "[rc3c1b][render-compiler][finalize][gpu]") {
    // The transform adds x_trace (a ray query of "scene"); the author's table
    // selects the declared `actors` for it. With the table the final IR has
    // x_trace on `actors`, waiting on its build; without the table, "scene"
    // and no such wait.
    Device device;
    const auto final_of = [&](bool table) {
        auto rendering = section(1, false);
        rendering["scene_acceleration_structures"] = Json::array({Json{{"name", "actors"}}});
        if (table) rendering["ray_task_acceleration_structures"] = {{"x_trace", "actors"}};
        Project project;
        Providers providers;
        providers.first.behavior = TransformBehavior::ray_task;
        const auto output = compile(project, providers, rendering, Variants::flat, true, &*device.backend);
        const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
        REQUIRE(flat.render_graph != nullptr);
        for (const auto &node : flat.render_graph->graphs.at(0).nodes) {
            if (node.name != "x_trace") continue;
            const auto &info = std::get<ComputeNodeInfo>(node.info);
            const bool waits = std::find(node.after.begin(), node.after.end(),
                                         "__scene_acceleration_structure:actors") != node.after.end();
            return std::pair{info.ray_query_acceleration_structure.value_or(""), waits};
        }
        FAIL("no x_trace in the final IR");
        return std::pair{std::string{}, false};
    };
    CHECK(final_of(false) == std::pair{std::string{"scene"}, false});
    CHECK(final_of(true) == std::pair{std::string{"actors"}, true});
}

TEST_CASE("RC-3c1b a table naming a task the preview leaves out leaves the preview unavailable",
          "[rc3c1b][render-compiler][preview][gpu]") {
    // A project feature with a history image and the ray task `trace`; the
    // preview leaves the feature out (history), the author's table still
    // names `trace`. The runtime variant compiles; the preview's IR, parsed
    // and resolved inside the preview's own try, refuses the table: the
    // preview is unavailable, the runtime stands.
    Device device;
    Project project;
    const auto feature_path = project.root / "features" / "rc3c1b_trace.json";
    fs::create_directories(feature_path.parent_path());
    auto accum = image("accum");
    accum["history"] = true;
    std::ofstream{feature_path, std::ios::binary}
        << Json{{"schema", "pelican.render_feature"},
                {"version", 1},
                {"name", "rc3c1b_trace"},
                {"runtime_shader_compiler", "optional"},
                {"render_targets", Json::array({accum})},
                {"buffers", Json::array({Json{{"name", "hits"}}})},
                {"compute_tasks", Json::array({rayQueryTask("trace", "scene")})}}
               .dump(2);
    auto rendering = section(0, false);
    rendering["features"] = Json::array({"project://features/rc3c1b_trace.json"});
    rendering["ray_task_acceleration_structures"] = {{"trace", "scene"}};
    Providers providers;
    const auto output = compile(project, providers, rendering, Variants::flat_and_preview, true, &*device.backend);
    const auto &flat = variantOf(output, RenderPipelineGraphVariant::flat);
    CHECK_FALSE(flat.unavailable);
    CHECK(flat.frame_plans.contains("main"));
    const auto &preview = variantOf(output, RenderPipelineGraphVariant::preview);
    REQUIRE(preview.unavailable);
    TestSupport::requireNamedRefusal([&] { std::rethrow_exception(preview.unavailable); },
                                     "render_graph.compute_task_invalid@1",
                                     {{"node", "trace"}, {"reason", "unknown_task"}});
}

namespace {

// RC-3c2b1: a record {value: <value>} as the u32 schema packs it.
std::vector<std::byte> packedValue(std::uint32_t value) {
    std::vector<std::byte> bytes(4);
    writeRayRecordU32(bytes, value);
    return bytes;
}

// Records as hex (Catch prints them).
std::vector<std::string> hexOf(const std::vector<std::vector<std::byte>> &records) {
    static constexpr char digits[] = "0123456789abcdef";
    std::vector<std::string> result;
    for (const auto &record : records) {
        std::string text;
        for (const auto byte : record) {
            text.push_back(digits[(static_cast<unsigned>(byte) >> 4) & 15]);
            text.push_back(digits[static_cast<unsigned>(byte) & 15]);
        }
        result.push_back(std::move(text));
    }
    return result;
}

const ComputeTaskDefinition &taskNamed(const std::vector<ComputeTaskDefinition> &tasks, const std::string &name) {
    const auto found = std::find_if(tasks.begin(), tasks.end(), [&](const auto &task) { return task.name == name; });
    REQUIRE(found != tasks.end());
    return *found;
}

struct ObservedComputeTasks {};

// RC-3c2b1 D3: what the production registration receives as each runtime
// variant's compute tasks (observe_compute_task_definitions, before anything
// is registered; then the registration stops), and the table entries and
// renames this thread applied while it compiled.
struct ReceivedComputeTasks {
    std::vector<std::vector<ComputeTaskDefinition>> variants;
    RenderGraphApplicationCounts applied;
    std::string refusal;
};

ReceivedComputeTasks receivedComputeTasks(const Json &rendering, bool with_xr) {
    FastModuleContainer modules;
    Project project;
    std::ofstream{project.root / "scene.json", std::ios::binary}
        << R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json";
    std::ofstream{project.root / "assets.json", std::ios::binary}
        << R"json({"schema":"pelican.asset_data","version":2,"models":[]})json";
    GET_MODULE(ProjectSource)
        .setSourceByData(Json{{"basic_config",
                               {{"window_size", {{"width", 16}, {"height", 16}}},
                                {"scene_data_json", "scene.json"},
                                {"asset_data_json", "assets.json"},
                                {"default_scene_id", "default_scene"}}}}
                             .dump());
    GET_MODULE(PathResolver).setup(project.root, false);
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = vk::Extent2D{16, 16};
    ReceivedComputeTasks result;
    std::vector<RenderingPassConfigRegistrationDependencies> runtime;
    {
        RenderingPassConfigRegistrationDependencies::Options flat;
        flat.gpu_owner_scope = renderPipelineVariantGpuOwnerScope(with_xr, RenderPipelineGraphVariant::flat);
        flat.observe_compute_task_definitions = [&result](auto variants) {
            result.variants.assign(variants.begin(), variants.end());
            throw ObservedComputeTasks{};
        };
        runtime.push_back(registrationDependencies(std::move(flat)));
#if PELICAN_WITH_OPENXR
        if (with_xr) {
            RenderingPassConfigRegistrationDependencies::Options xr;
            xr.graph_variant = RenderPipelineGraphVariant::xr;
            xr.publish_enabled_features = false;
            xr.gpu_owner_scope = renderPipelineVariantGpuOwnerScope(true, RenderPipelineGraphVariant::xr);
            runtime.push_back(registrationDependencies(std::move(xr)));
        }
#endif
    }
    const auto document = project.rootOf(rendering).dump();
    const auto before = renderGraphApplicationCounts();
    try {
        (void)registerRenderGraphVariantFamilyFromJsonData(document, {16, 16}, std::move(runtime));
        result.refusal = "published without reaching the observation point";
    } catch (const ObservedComputeTasks &) {
    } catch (const std::exception &error) {
        result.refusal = error.what();
    }
    const auto after = renderGraphApplicationCounts();
    result.applied = {
        .acceleration_structure_entries =
            after.acceleration_structure_entries - before.acceleration_structure_entries,
        .callable_record_entries = after.callable_record_entries - before.callable_record_entries,
        .compute_task_renames = after.compute_task_renames - before.compute_task_renames,
    };
    return result;
}

} // namespace

TEST_CASE("RC-3c2b1 D3 the registration receives each table and the variant's names applied once, from the IR",
          "[rc3c2b1][registration][compute][gpu]") {
    // Two config tasks: `query` (a ray query of `actors`) and `trace` (a ray
    // tracing task without a ray group layout, tracing `actors`, with one
    // callable and its inline record {value: 1}). With the tables, both
    // trace `props` and trace's record is {value: 9}; without, what they
    // declare. Each table entry is applied once per variant's final revision
    // (flat, XR, and the preview's references), each XR name once; the
    // definitions are what the IR holds after that.
    setupLogger();
    auto rendering = section(0, false);
    rendering["scene_acceleration_structures"] = Json::array({Json{{"name", "actors"}}, Json{{"name", "props"}}});
    rendering["buffers"] = Json::array({Json{{"name", "hits"}}});
    rendering["compute_tasks"] = Json::array({
        rayQueryTask("query", "actors"),
        Json{{"name", "trace"},
             {"ray_tracing",
              {{"raygen", "shaders/trace"},
               {"miss", "shaders/trace"},
               {"closesthit", "shaders/trace"},
               {"callable", Json::array({"shaders/callable"})},
               {"callable_record", Json::array({Json{{"name", "value"}, {"type", "u32"}}})},
               {"callable_records", Json::array({Json{{"value", 1}}})},
               {"acceleration_structure", "actors"}}},
             {"dispatch", {{"rays", Json::array({1, 1, 1})}}}},
    });
    auto with_tables = rendering;
    with_tables["ray_task_acceleration_structures"] = {{"query", "props"}, {"trace", "props"}};
    with_tables["ray_task_callable_records"] = {{"trace", Json::array({Json{{"value", 9}}})}};
#if PELICAN_WITH_OPENXR
    constexpr bool xr = true;
    const std::vector<std::string> suffixes{"", "#xr"};
#else
    constexpr bool xr = false;
    const std::vector<std::string> suffixes{""};
#endif
    // The finalized runtime variants and the preview.
    const std::size_t revisions = suffixes.size() + 1;
    for (const bool tables : {false, true}) {
        DYNAMIC_SECTION("tables " << tables) {
            const auto received = receivedComputeTasks(tables ? with_tables : rendering, xr);
            INFO(received.refusal);
            REQUIRE(received.refusal.empty());
            REQUIRE(received.variants.size() == suffixes.size());
            const std::string structure = tables ? "props" : "actors";
            for (std::size_t index = 0; index < suffixes.size(); ++index) {
                INFO("variant " << index);
                const auto &tasks = received.variants[index];
                REQUIRE(tasks.size() == 2);
                const auto &query = taskNamed(tasks, "query" + suffixes[index]);
                CHECK(query.ray_query_acceleration_structure == std::optional{structure});
                const auto &trace = taskNamed(tasks, "trace" + suffixes[index]);
                REQUIRE(trace.ray_tracing);
                CHECK(trace.ray_tracing->acceleration_structure == structure);
                REQUIRE(trace.ray_tracing->callable_record_layout);
                CHECK(trace.ray_tracing->callable_record_layout->data_size == 4);
                CHECK(hexOf(trace.ray_tracing->callable_records) == hexOf({packedValue(tables ? 9 : 1)}));
                CHECK(trace.ray_tracing->callables.size() == 1);
            }
            CHECK(received.applied.acceleration_structure_entries == (tables ? 2 * revisions : 0));
            CHECK(received.applied.callable_record_entries == (tables ? revisions : 0));
            // XR's suffix on the two tasks, once.
            CHECK(received.applied.compute_task_renames == (suffixes.size() - 1) * 2);
        }
    }
    // The variant's names: flat alone renames nothing.
    const auto flat_only = receivedComputeTasks(with_tables, false);
    INFO(flat_only.refusal);
    REQUIRE(flat_only.variants.size() == 1);
    CHECK(taskNamed(flat_only.variants.front(), "trace").ray_tracing->acceleration_structure == "props");
    CHECK(flat_only.applied.compute_task_renames == 0);
    // The preview's own IR, as the program returns it (it is not finalized:
    // its references are resolved, the tables applied, in its own try):
    // the same values the runtime variants register.
    Device device;
    for (const bool tables : {false, true}) {
        DYNAMIC_SECTION("preview, tables " << tables) {
            Project project;
            Providers providers;
            const auto output = compile(project, providers, tables ? with_tables : rendering,
                                        Variants::flat_and_preview, false, &*device.backend);
            const auto &preview = variantOf(output, RenderPipelineGraphVariant::preview);
            REQUIRE_FALSE(preview.unavailable);
            REQUIRE(preview.render_graph != nullptr);
            std::map<std::string, const ComputeNodeInfo *> infos;
            for (const auto &task : sharedComputeTasksOf(*preview.render_graph))
                infos.emplace(task.node->name, &std::get<ComputeNodeInfo>(task.node->info));
            REQUIRE(infos.size() == 2);
            const std::string structure = tables ? "props" : "actors";
            CHECK(infos.at("query")->ray_query_acceleration_structure == std::optional{structure});
            REQUIRE(infos.at("trace")->ray_tracing);
            CHECK(infos.at("trace")->ray_tracing->acceleration_structure == structure);
            CHECK(hexOf(infos.at("trace")->ray_tracing->callable_records) == hexOf({packedValue(tables ? 9 : 1)}));
        }
    }
}

TEST_CASE("RC-3c2b1 the callable records' table names a task a provider adds; the preview without it is unavailable",
          "[rc3c2b1][render-compiler][preview][compute][gpu]") {
    // The transform adds x_call (a ray tracing task with one callable and
    // its inline record {value: 1}); the author's table gives it {value: 7}.
    // The runtime variant's final IR resolves the table (the provider's
    // task); the preview, which no provider reaches, resolves the same table
    // in its own references and has no such task: unavailable, by name.
    Device device;
    const auto compiled = [&](bool table) {
        auto rendering = section(1, false);
        if (table) rendering["ray_task_callable_records"] = {{"x_call", Json::array({Json{{"value", 7}}})}};
        Project project;
        Providers providers;
        providers.first.behavior = TransformBehavior::ray_callable_task;
        return compile(project, providers, rendering, Variants::flat_and_preview, true, &*device.backend);
    };
    for (const bool table : {false, true}) {
        DYNAMIC_SECTION("table " << table) {
            const auto output = compiled(table);
            REQUIRE(output.compute_task_definitions.size() == 1);
            const auto &call = taskNamed(output.compute_task_definitions.front(), "x_call");
            REQUIRE(call.ray_tracing);
            CHECK(hexOf(call.ray_tracing->callable_records) == hexOf({packedValue(table ? 7 : 1)}));
            const auto &preview = variantOf(output, RenderPipelineGraphVariant::preview);
            if (!table) {
                CHECK_FALSE(preview.unavailable);
                continue;
            }
            REQUIRE(preview.unavailable);
            TestSupport::requireNamedRefusal([&] { std::rethrow_exception(preview.unavailable); },
                                             "render_graph.compute_task_invalid@1",
                                             {{"node", "x_call"},
                                              {"field", "ray_task_callable_records"},
                                              {"reason", "unknown_task"}});
        }
    }
}

} // namespace Pelican
