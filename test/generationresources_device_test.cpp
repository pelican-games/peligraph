// RC-3c2a: the generation's resource description through the production
// registration on a device. C1: a flat and an XR variant (the XR one exports
// its depth) with three alias groups -- one both variants place, one the XR
// variant has half of, one the XR variant has none of -- registered and
// published, and the same document registered by flat alone (the merge's
// other input absent) in the same test: what the resource checks read (the
// description) and what the GPU registration made (the registry) carry the
// merged usage, layers and alias groups, and the allocations they imply; flat
// alone carries none of what the XR variant adds. C2: a directional shadow
// target's layer count follows the light count at runtime, inside the static
// constraint the description gives it, and the runtime check refuses a view
// count past it.
#include "rendercompilerprogram_ir_test_support.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/light/lightcontainer.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/log.hpp"
#include "../src/core/renderer/debugdraw.hpp"
#include "../src/core/renderer/debugtext.hpp"
#include "../src/core/renderer/directionalshadowviewprovider.hpp"
#include "../src/core/renderer/shadowdepthpasscontainer.hpp"
#include "../src/core/renderer/velocitypasscontainer.hpp"
#include "../src/core/renderer/viewfamily.hpp"
#include "../src/core/renderer/viewfamilyproviderregistry.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/shader/shaderlibrary.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer_config.hpp"
#include "../src/core/vkcore/rendertarget.hpp"
#include "../src/project/generationresources.hpp"
#include "../src/project/viewfamilyrelation.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <glm/gtc/matrix_transform.hpp>

#include <fstream>
#include <map>
#include <set>

namespace Pelican {
namespace {

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

// A headless engine in `project`'s root at `extent`.
void startHeadless(const Project &project, vk::Extent2D extent) {
    std::ofstream{project.root / "scene.json", std::ios::binary}
        << R"json({"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[]}}})json";
    std::ofstream{project.root / "assets.json", std::ios::binary}
        << R"json({"schema":"pelican.asset_data","version":2,"models":[]})json";
    GET_MODULE(ProjectSource)
        .setSourceByData(Json{{"basic_config",
                               {{"window_size", {{"width", extent.width}, {"height", extent.height}}},
                                {"scene_data_json", "scene.json"},
                                {"asset_data_json", "assets.json"},
                                {"default_scene_id", "default_scene"}}}}
                             .dump());
    GET_MODULE(PathResolver).setup(project.root, false);
    auto &launch = GET_MODULE(EngineLaunchConfig);
    launch.headless = true;
    launch.headless_extent = extent;
    (void)GET_MODULE(RenderTarget).getExtent();
}

Json target(const std::string &name, const std::string &format,
            Json usage = Json::array({"COLOR_ATTACHMENT", "SAMPLED"})) {
    return Json{{"name", name}, {"extent_scale", 1.0}, {"format", format}, {"usage", std::move(usage)}};
}

// A fullscreen pass of the engine's present shader (it samples its first
// input; a multiview-capable builtin, so the XR variant runs it with
// multiview).
Json pass(const std::string &name, const std::vector<std::string> &input, const std::string &output) {
    Json result{{"name", name},
                {"type", "fullscreen"},
                {"output", {{"color", Json::array({Json{{"target", output}}})}, {"depth", nullptr}}},
                {"shader", {{"vertex", "engine://fullscreen"}, {"fragment", "engine://scene_present"}}}};
    if (!input.empty()) result["input"] = input;
    return result;
}

// One chain: <group>1 written (from the scene), read into a mid image, the mid read into
// <group>2, <group>2 read into an end image. The members (format `format`,
// the alias-safe usage) do not overlap; the mid and end images carry
// TRANSFER_SRC, which keeps them out of aliasing.
void addChain(Json &targets, Json &passes, const std::string &group, const std::string &format,
              bool first_half, bool second_half) {
    const Json kept = Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"});
    if (first_half) {
        targets.push_back(target(group + "1", format));
        targets.push_back(target(group + "_mid", "R16G16B16A16_SFLOAT", kept));
        passes.push_back(pass(group + "_write", {"scene"}, group + "1"));
        passes.push_back(pass(group + "_mid", {group + "1"}, group + "_mid"));
    }
    if (second_half) {
        targets.push_back(target(group + "2", format));
        targets.push_back(target(group + "_end", "R16G16B16A16_SFLOAT", kept));
        passes.push_back(pass(group + "_second", {group + "_mid"}, group + "2"));
        passes.push_back(pass(group + "_end", {group + "2"}, group + "_end"));
    }
}

struct Registered {
    GenerationResources resources;
    struct Target {
        RenderTargetMetadata metadata;
        GlobalRenderTargetId id;
    };
    std::map<std::string, Target> targets;
    std::string refusal;
    std::vector<std::set<std::string>> shared;
    std::size_t allocations = 0;
    std::uint64_t allocated_bytes = 0;
};

// One production registration of the C1 document: flat (and the XR variant
// as an XR launch registers it, when `with_xr`), in an engine of its own.
// What the checks and the GPU registration received: the description the
// compiler program's completion made and checked (the registration hands it
// on unchanged: observe_generation_resources), and the registry before
// publication.
Registered registerC1(bool with_xr) {
    FastModuleContainer modules;
    Project project;
    startHeadless(project, {64, 64});

    // flat: draw (the scene and its depth), present, the chains a (both
    // halves) and b (first half) in the root, b's second half and the whole
    // chain c in a project feature the XR variant leaves out (a projection
    // jitter provider).
    Json targets = Json::array({target("scene", "R16G16B16A16_SFLOAT"),
                                target("depth", "D32_SFLOAT", Json::array({"DEPTH_STENCIL_ATTACHMENT"}))});
    Json passes = Json::array({Json{{"name", "draw"},
                                    {"type", "material"},
                                    {"material_contract", "forward_opaque_v1"},
                                    {"output", {{"color", Json::array({"scene"})}, {"depth", "depth"}}}},
                               pass("present", {"scene"}, "swapchain")});
    addChain(targets, passes, "a", "R8_UNORM", true, true);
    addChain(targets, passes, "b", "R16_SFLOAT", true, false);
    Json feature_targets = Json::array();
    Json feature_passes = Json::array();
    addChain(feature_targets, feature_passes, "b", "R16_SFLOAT", false, true);
    addChain(feature_targets, feature_passes, "c", "R32_SFLOAT", true, true);
    Json inserted = Json::array();
    for (auto &entry : feature_passes) inserted.push_back(Json{{"insert", "end"}, {"pass", std::move(entry)}});
    const auto feature_path = project.root / "features" / "rc3c2a_flat_only.json";
    fs::create_directories(feature_path.parent_path());
    std::ofstream{feature_path, std::ios::binary}
        << Json{{"schema", "pelican.render_feature"},
                {"version", 1},
                {"name", "rc3c2a_flat_only"},
                {"runtime_shader_compiler", "optional"},
                {"projection_jitter", {{"pattern", "halton23"}, {"phases", 8}}},
                {"render_targets", feature_targets},
                {"passes", inserted}}
               .dump(2);
    Json section{{"features", Json::array({"project://features/rc3c2a_flat_only.json"})},
                 {"render_targets", targets},
                 {"rendering_passes", Json::array({Json{{"name", "main"}, {"passes", passes}}})}};
    // The XR variant runs its fullscreen passes with multiview: a measured
    // gain on this device (a profile with its vendor and device id), as the
    // RC-3c1b registration test has it. Its images then hold both views.
    const auto properties = GET_MODULE(VulkanManageCore).getPhysDevice().getProperties();
    section["xr"] = {
        {"view_execution", "auto"},
        {"multiview_auto",
         {{"profiles", Json::array({Json{{"id", "rc3c2a_this_device"},
                                         {"vendor_id", properties.vendorID},
                                         {"device_id", properties.deviceID},
                                         {"measurement",
                                          {{"sequential_gpu_ms", 6.0},
                                           {"multiview_gpu_ms", 4.0},
                                           {"sample_count", 240},
                                           {"source", "RenderTiming/gpu_timestamp"}}}}})}}}};

    Registered result;
    RenderingPassConfigRegistrationDependencies::Options flat;
    flat.gpu_owner_scope = renderPipelineVariantGpuOwnerScope(with_xr, RenderPipelineGraphVariant::flat);
    flat.observe_generation_resources = [&](const GenerationResources &resources) {
        result.resources = resources;
    };
    flat.validate_prepared_generation = [&](const RendererRuntimeGeneration &) {
        auto &container = GET_MODULE(RenderTargetContainer);
        for (const auto &image : result.resources.images) {
            const auto id = container.getRenderTargetIdByName(image.declaration.name);
            REQUIRE(isConcreteRenderTarget(id));
            result.targets.emplace(image.declaration.name, Registered::Target{container.getMetadata(id), id});
        }
        std::set<std::string> counted;
        for (const auto &[name, entry] : result.targets) {
            if (counted.contains(name)) continue;
            std::set<std::string> members{name};
            auto bytes = renderTargetBytes(entry.metadata);
            for (const auto &[other, other_entry] : result.targets) {
                if (other == name || !container.sharesAllocation(entry.id, other_entry.id)) continue;
                members.insert(other);
                bytes = std::max(bytes, renderTargetBytes(other_entry.metadata));
            }
            counted.insert(members.begin(), members.end());
            ++result.allocations;
            result.allocated_bytes += bytes;
            if (members.size() > 1) result.shared.push_back(members);
        }
    };
    std::vector<RenderingPassConfigRegistrationDependencies> runtime;
    runtime.push_back(registrationDependencies(std::move(flat)));
#if PELICAN_WITH_OPENXR
    if (with_xr) {
        RenderingPassConfigRegistrationDependencies::Options xr;
        xr.graph_variant = RenderPipelineGraphVariant::xr;
        xr.enable_multiview_runtime = true;
        xr.enable_external_depth_export = true;
        xr.publish_enabled_features = false;
        xr.gpu_owner_scope = renderPipelineVariantGpuOwnerScope(true, RenderPipelineGraphVariant::xr);
        runtime.push_back(registrationDependencies(std::move(xr)));
    }
#endif
    try {
        (void)registerRenderGraphVariantFamilyFromJsonData(project.rootOf(section).dump(), {64, 64},
                                                           std::move(runtime));
    } catch (const std::exception &error) {
        result.refusal = error.what();
    }
    return result;
}

} // namespace

TEST_CASE("RC-3c2a C1: the registration merges the variants' images once, for the checks and the GPU",
          "[rc3c2a][registration][generation-resources][gpu]") {
#if PELICAN_WITH_OPENXR
    setupLogger();
    // The same document twice: with the XR variant (its export, its two
    // views, its half of the group b merged in) and without it (the merge's
    // other input absent: what flat alone makes).
    const auto merged = registerC1(true);
    const auto alone = registerC1(false);
    INFO(merged.refusal << " / " << alone.refusal);
    REQUIRE(merged.refusal.empty());
    REQUIRE(alone.refusal.empty());
    REQUIRE(merged.resources.variants.size() == 2);
    REQUIRE(alone.resources.variants.size() == 1);
    const auto in_variant = [&](std::size_t variant, std::string_view name) {
        for (const auto index : merged.resources.variants[variant].images)
            if (merged.resources.images[index].declaration.name == name) return true;
        return false;
    };
    // The XR variant left the feature out: b2 and the chain c are flat's.
    CHECK(in_variant(0, "b2"));
    CHECK_FALSE(in_variant(1, "b2"));
    CHECK(in_variant(1, "b1"));
    CHECK_FALSE(in_variant(1, "c1"));
    CHECK_FALSE(in_variant(1, "c2"));

    const auto described = [](const Registered &run, std::string_view name) -> const GenerationImage & {
        const auto *image = run.resources.findImage(name);
        REQUIRE(image != nullptr);
        return *image;
    };
    const auto registered = [](const Registered &run, const std::string &name) -> const RenderTargetMetadata & {
        REQUIRE(run.targets.contains(name));
        return run.targets.at(name).metadata;
    };
    // What the GPU registration made is what the checks read: every image of
    // both runs, field by field.
    for (const auto *run : {&merged, &alone}) {
        for (const auto &[name, entry] : run->targets) {
            INFO(name);
            const auto metadata = renderTargetMetadataOf(described(*run, name));
            CHECK(metadata.usage == entry.metadata.usage);
            CHECK(metadata.format == entry.metadata.format);
            CHECK(metadata.extent == entry.metadata.extent);
            CHECK(metadata.samples == entry.metadata.samples);
            CHECK(metadata.array_layers == entry.metadata.array_layers);
            CHECK(metadata.alias_group == entry.metadata.alias_group);
            CHECK(metadata.storage_mode == entry.metadata.storage_mode);
        }
    }

    // The usage union: the XR export's TRANSFER_SRC on the depth flat
    // registers first, materialized (no transient bit). Flat alone: no
    // TRANSFER_SRC.
    const ImageUsage depth_usage = ImageUsageBit::depth_stencil_attachment | ImageUsageBit::transfer_src;
    CHECK(described(merged, "depth").usage == depth_usage);
    CHECK(described(merged, "depth").storage == RenderTargetStorageMode::materialized);
    CHECK(registered(merged, "depth").usage == depth_usage);
    CHECK(registered(merged, "depth").format == ImageFormat::d32_sfloat);
    CHECK(registered(merged, "depth").extent == ResolvedResourceExtent{64, 64});
    CHECK(registered(merged, "depth").samples == 1);
    CHECK_FALSE(registered(alone, "depth").usage & ImageUsageBit::transfer_src);
    // The layers' maximum: the XR variant's two views; flat alone: one.
    for (const auto *name : {"scene", "depth", "a1", "b1"}) {
        INFO(name);
        CHECK(registered(merged, name).array_layers == 2);
        CHECK(registered(alone, name).array_layers == 1);
    }
    CHECK(registered(merged, "c1").array_layers == 1);
    // The groups: a stands, b leaves (XR has b1 without b2), c stands (XR has
    // neither). Flat alone keeps all three.
    const auto group = [&](const Registered &run, const std::string &name) {
        return registered(run, name).alias_group;
    };
    CHECK(group(merged, "a1").has_value());
    CHECK(group(merged, "a1") == group(merged, "a2"));
    CHECK_FALSE(group(merged, "b1").has_value());
    CHECK_FALSE(group(merged, "b2").has_value());
    CHECK(group(merged, "c1") == group(merged, "c2"));
    CHECK(group(alone, "b1").has_value());
    CHECK(group(alone, "b1") == group(alone, "b2"));
    using Shared = std::vector<std::set<std::string>>;
    CHECK(merged.shared == Shared{{"a1", "a2"}, {"c1", "c2"}});
    CHECK(alone.shared == Shared{{"a1", "a2"}, {"b1", "b2"}, {"c1", "c2"}});
    // 15 images (the output's display image with them): two pairs sharing,
    // 13 allocations; flat alone, three pairs, 12.
    std::set<std::string> names;
    for (const auto &[name, entry] : merged.targets) names.insert(name);
    CHECK(names == std::set<std::string>{"a1", "a2", "a_end", "a_mid", "b1", "b2", "b_end", "b_mid", "c1",
                                         "c2", "c_end", "c_mid", "depth", "display", "scene"});
    CHECK(merged.allocations == 13);
    CHECK(alone.allocations == 12);
    std::uint64_t expected_bytes = 0;
    for (const auto &[name, entry] : merged.targets)
        if (name != "a2" && name != "c2") expected_bytes += renderTargetBytes(entry.metadata);
    CHECK(merged.allocated_bytes == expected_bytes);
    CHECK(renderTargetBytes(registered(merged, "a1")) == 64u * 64u * 1u * 2u);
    CHECK(renderTargetBytes(registered(merged, "c1")) == 64u * 64u * 4u);
#else
    SKIP("an XR variant needs PELICAN_WITH_OPENXR");
#endif
}

TEST_CASE("RC-3c2a C2: a directional shadow target's layers follow the light count inside its constraint",
          "[rc3c2a][registration][generation-resources][shadow][gpu]") {
    setupLogger();
    FastModuleContainer modules;
    Project project;
    startHeadless(project, {64, 64});
    const Json section{
        {"pipeline", {{"preset", "engine://render_pipelines/hybrid_v1.json"}}},
        {"features", Json::array({Json{{"ref", "engine://features/shadow_directional.json"},
                                       {"parameters", {{"cascade_count", 2}}}}})}};
    GenerationResources resources;
    RenderingPassConfigRegistrationDependencies::Options options;
    options.observe_generation_resources = [&](const GenerationResources &described) { resources = described; };
    std::vector<RenderingPassConfigRegistrationDependencies> runtime;
    runtime.push_back(registrationDependencies(std::move(options)));
    std::string refusal;
    try {
        (void)registerRenderGraphVariantFamilyFromJsonData(project.rootOf(section).dump(), {64, 64},
                                                           std::move(runtime));
    } catch (const std::exception &error) {
        refusal = error.what();
    }
    INFO(refusal);
    REQUIRE(refusal.empty());

    // The static constraint: the shadow map's layers are the runtime's (no
    // alias group, 2D, no history); the description holds the plan's.
    const auto *shadow = resources.findImage("shadow_map");
    REQUIRE(shadow != nullptr);
    CHECK(shadow->runtime_array_layers);
    CHECK_FALSE(shadow->alias_group);
    CHECK_FALSE(shadow->declaration.history);
    CHECK(shadow->declaration.dimension == ImageResourceDimension::two_d);

    const auto generation = GET_MODULE(FrameGraphRuntimeContainer).snapshot();
    REQUIRE(generation != nullptr);
    REQUIRE(generation->programs.size() == 1);
    const auto &frame_graph = generation->programs.begin()->second.frame_graph;
    auto &targets = GET_MODULE(RenderTargetContainer);
    auto &lights = GET_MODULE(LightContainer);
    const auto shadow_id = frame_graph.render_target_bindings.at("shadow_map");
    const auto load_lights = [&](std::size_t count) {
        std::vector<LightLoadEntry> entries;
        for (std::size_t index = 0; index < count; ++index)
            entries.push_back(LightLoadEntry{
                .name = "rc3c2a_sun_" + std::to_string(index),
                .component = LightCodecData{.type = LightCodecType::Directional,
                                            .direction = {0.5F, 0, -1},
                                            .intensity = 1,
                                            .color = {1, 1, 1}}});
        lights.load(entries);
    };
    const auto *provider =
        renderViewFamilyProviderRegistry().find(std::string{directionalShadowRenderViewFamilyId});
    REQUIRE(provider != nullptr);
    const auto build = [&] {
        const RenderViewFamilyProviderContext context{frame_graph, lights, targets};
        // The main view (a camera at the origin) the cascades fit.
        RenderViewParameters camera;
        camera.projection = glm::perspective(0.8f, 1.0f, 0.1f, 100.0f);
        camera.depth_range = RenderViewDepthRange{0.1f, 100.0f};
        const RenderViewFamilies families{.families = {makeMainRenderViewFamily(camera)}};
        return provider->build(families, context).views.size();
    };
    // Three suns, two cascades: the runtime resizes the target to six layers
    // and the family's six views fit it.
    load_lights(3);
    CHECK(prepareDirectionalShadowRenderTarget(frame_graph, lights, targets));
    CHECK(targets.getMetadata(shadow_id).array_layers == 6);
    CHECK(build() == 6);
    // A fourth sun without the resize: the runtime check refuses the views
    // past the layers.
    load_lights(4);
    CHECK_THROWS_WITH(build(), Catch::Matchers::ContainsSubstring(
                                   "directional shadow target has fewer array layers than the runtime shadow "
                                   "view count"));
    CHECK(prepareDirectionalShadowRenderTarget(frame_graph, lights, targets));
    CHECK(targets.getMetadata(shadow_id).array_layers == 8);
    CHECK(build() == 8);
    // The same count again: nothing to resize.
    CHECK_FALSE(prepareDirectionalShadowRenderTarget(frame_graph, lights, targets));
}

} // namespace Pelican
