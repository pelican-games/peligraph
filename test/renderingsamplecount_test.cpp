#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "registered_render_targets_test_support.hpp"
#include "render_graph_ir_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {
namespace {

// RC-3c1b: the physical plan reads each graph's images from the IR (the
// logical graph's types and materialization) beside the execution
// declarations. A test that builds its RenderTargetDefinitions by hand gives
// the IR the same images (a copy of each definition as the IR's image).
RenderGraphIR withImagesOf(RenderGraphIR ir, std::span<const RenderTargetDefinition> targets) {
    ir.images.clear();
    for (const auto &target : targets) {
        ImageResource image;
        image.name = target.name;
        image.format_class = target.format_class;
        image.role = target.role;
        image.format = target.format;
        image.format_candidates = {target.format};
        for (const auto format : target.format_candidates)
            if (std::find(image.format_candidates.begin(), image.format_candidates.end(), format) ==
                image.format_candidates.end())
                image.format_candidates.push_back(format);
        image.usage = target.usage;
        image.extent = target.fixed_extent
                           ? ResourceExtentPlan{.kind = ResourceExtentKind::fixed,
                                                .width = target.fixed_extent->width,
                                                .height = target.fixed_extent->height}
                           : ResourceExtentPlan{.kind = ResourceExtentKind::output_relative,
                                                .scale_x = target.extent_scale,
                                                .scale_y = target.extent_scale};
        image.mip_levels = target.mip_levels;
        image.array_layers = target.array_layers;
        image.dimension = target.dimension;
        image.history = target.history;
        image.persistent = target.persistent;
        image.source = target.source;
        ir.images.push_back(std::move(image));
    }
    return ir;
}

// RC-3c2a: `targets` as one variant's GPU registration takes them from
// `compilation` (imagePlacementsOf, then the generation's resource
// description over the IR of these declarations).
void applyPlan(std::vector<RenderTargetDefinition> &targets,
               const RenderingTargetPlanCompilation &compilation) {
    targets = test_support::plannedRenderTargets(withImagesOf(RenderGraphIR{}, targets), compilation);
}

RenderTargetDefinition target(
    std::string name, ImageFormat format,
    ImageUsage usage =
        ImageUsageBit::color_attachment |
        ImageUsageBit::sampled) {
    return {
        .name = std::move(name),
        .format_class = "data",
        .role = "data",
        .format = format,
        .usage = usage,
    };
}

const RenderingSampleCountAssignment &assignment(
    const RenderingTargetPlanCompilation &compilation,
    std::string_view resource) {
    const auto found = std::find_if(
        compilation.assignments.begin(),
        compilation.assignments.end(),
        [resource](const auto &candidate) {
            return candidate.resource == resource;
        });
    REQUIRE(found != compilation.assignments.end());
    return *found;
}

const RenderingTargetArrayLayerAssignment &layerAssignment(
    const RenderingTargetPlanCompilation &compilation,
    std::string_view resource) {
    const auto found = std::find_if(
        compilation.array_layer_assignments.begin(),
        compilation.array_layer_assignments.end(),
        [resource](const auto &candidate) {
            return candidate.resource == resource;
        });
    REQUIRE(found != compilation.array_layer_assignments.end());
    return *found;
}

const RenderingTargetFormatAssignment &formatAssignment(
    const RenderingTargetPlanCompilation &compilation,
    std::string_view resource) {
    const auto found = std::find_if(
        compilation.format_assignments.begin(),
        compilation.format_assignments.end(),
        [resource](const auto &candidate) {
            return candidate.resource == resource;
        });
    REQUIRE(
        found !=
        compilation.format_assignments.end());
    return *found;
}

const RenderingTargetRepresentationAssignment &
representationAssignment(
    const RenderingTargetPlanCompilation &compilation,
    std::string_view resource) {
    const auto found = std::find_if(
        compilation.representation_assignments.begin(),
        compilation.representation_assignments.end(),
        [resource](const auto &candidate) {
            return candidate.resource == resource;
        });
    REQUIRE(
        found !=
        compilation.representation_assignments.end());
    return *found;
}

const VulkanPhysicalResourcePlan &physicalResource(
    const VulkanTargetPlan &plan, std::string_view resource) {
    const auto found = std::find_if(
        plan.resources.begin(), plan.resources.end(),
        [resource](const auto &candidate) {
            return candidate.logical_resource == resource;
        });
    REQUIRE(found != plan.resources.end());
    return *found;
}

const VulkanPhysicalAttachmentPlan &physicalAttachment(
    const VulkanTargetPlan &plan,
    std::string_view node,
    std::string_view resource) {
    const auto found = std::find_if(
        plan.attachments.begin(),
        plan.attachments.end(),
        [&](const auto &candidate) {
            return candidate.node == node &&
                   candidate.logical_resource ==
                       resource;
        });
    REQUIRE(found != plan.attachments.end());
    return *found;
}

void requireThrowsContaining(const std::function<void()> &operation,
                             std::string_view expected) {
    try {
        operation();
        FAIL("operation did not throw");
    } catch (const std::runtime_error &error) {
        INFO(error.what());
        REQUIRE(std::string_view{error.what()}.find(expected) !=
                std::string_view::npos);
    }
}

nlohmann::json hybridConfig() {
    return {
        {"multisampling",
         {{"fallback", "lower_supported"},
          {"samples", 4},
          {"scope", "geometry"}}},
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "main"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "geometry"},
                      {"type", "material"},
                      {"output",
                       {{"color",
                         nlohmann::json::array(
                             {"albedo", "normal", "custom_id"})},
                        {"depth", "depth"}}}},
                     // RC-3c1b: the planner orders the writers of lit
                     // and depth (the physical plan plans the graph first).
                     {{"name", "lighting"},
                      {"type", "fullscreen"},
                      {"after", nlohmann::json::array({"geometry"})},
                      {"output",
                       {{"color", nlohmann::json::array({"lit"})},
                        {"depth", nullptr}}}},
                     {{"name", "forward"},
                      {"type", "material"},
                      {"after", nlohmann::json::array({"lighting"})},
                      {"output",
                       {{"color", nlohmann::json::array({"lit"})},
                        {"depth", "depth"}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", nlohmann::json::array({"display"})},
                        {"depth", nullptr}}}}})}}})},
    };
}

nlohmann::json writeOnlyAttachmentConfig() {
    return {
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "transient"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "transient_probe"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", "scratch"},
                        {"depth", nullptr}}}}})}}})},
    };
}

nlohmann::json localReadAttachmentConfig(
    std::string consumer_type = "fullscreen",
    float gbuffer_extent_scale = 1.0f) {
    auto result = nlohmann::json{
        {"render_targets",
         nlohmann::json::array({
             {
                 {"name", "gbuffer"},
                 {"format", "R8G8B8A8_UNORM"},
                 {"usage",
                  nlohmann::json::array(
                      {"COLOR_ATTACHMENT",
                       "SAMPLED"})},
                 {"extent_scale",
                  gbuffer_extent_scale},
             },
             {
                 {"name", "lit"},
                 {"extent_scale", 1.0},
                 {"format", "R8G8B8A8_UNORM"},
                 {"usage",
                  nlohmann::json::array(
                      {"COLOR_ATTACHMENT",
                       "SAMPLED"})},
             },
             {
                 {"name", "display"},
                 {"extent_scale", 1.0},
                 {"format", "R8G8B8A8_UNORM"},
                 {"usage",
                  nlohmann::json::array(
                      {"COLOR_ATTACHMENT",
                       "SAMPLED"})},
             },
         })},
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "local_read"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "geometry"},
                      {"type", "material"},
                      {"output",
                       {{"color", "gbuffer"},
                        {"depth", nullptr}}}},
                     {{"name", "lighting"},
                      {"type", consumer_type},
                      {"input", "gbuffer"},
                      {"input_footprints",
                       {{"gbuffer", "same_pixel"}}},
                      {"output",
                       {{"color", "lit"},
                        {"depth", nullptr}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"input", "lit"},
                      {"output",
                       {{"color", "display"},
                        {"depth", nullptr}}}}})}}})},
    };
    if (consumer_type == "material") {
        auto &consumer =
            result["rendering_passes"][0]
                  ["passes"][1];
        consumer.erase("input");
        consumer.erase("input_footprints");
        consumer["material_resources"] = {
            {"gbuffer_input",
             {
                 {"resource", "gbuffer"},
                 {"kind", "image"},
                 {"access", "sampled"},
                 {"footprint", "same_pixel"},
             }},
        };
    } else if (consumer_type == "output_transform") {
        result["rendering_passes"][0]
              ["passes"][1]["output"]["color"] =
            "display";
        result["rendering_passes"][0]
              ["passes"].erase(2);
    }
    return result;
}

nlohmann::json aliasLifetimeConfig(
    bool include_second_temporary = true) {
    auto passes = nlohmann::json::array(
        {{{"name", "produce_a"},
          {"type", "fullscreen"},
          {"output",
           {{"color", "temporary_a"},
            {"depth", nullptr}}}},
         {{"name", "consume_a"},
          {"type", "fullscreen"},
          {"input", "temporary_a"},
          {"input_footprints",
           {{"temporary_a", "arbitrary"}}},
          {"output",
           {{"color", "display"},
            {"depth", nullptr}}}}});
    if (include_second_temporary) {
        passes.push_back(
            {{"name", "produce_b"},
             {"type", "fullscreen"},
             {"after",
              nlohmann::json::array({"consume_a"})},
             {"output",
              {{"color", "temporary_b"},
               {"depth", nullptr}}}});
        passes.push_back(
            {{"name", "consume_b"},
             {"type", "fullscreen"},
             {"input", "temporary_b"},
             {"input_footprints",
              {{"temporary_b", "arbitrary"}}},
             {"output",
              {{"color", "display"},
               {"depth", nullptr}}}});
    }
    return {
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name",
                include_second_temporary
                    ? "alias_lifetimes"
                    : "single_temporary"},
               {"passes", std::move(passes)}}})},
    };
}

RenderingTargetPlanDeviceFacts
localReadDeviceFacts(bool feature = true,
                     bool format = true) {
    return {
        .query_image_format_capability =
            [format](
                const RenderTargetDefinition &) {
                return RenderingImageFormatCapability{
                    .image_usage_supported = true,
                    .supported_samples = {1, 2, 4},
                    .max_array_layers = 4,
                    .transient_attachment_supported =
                        true,
                    .local_read_attachment_supported =
                        format,
                };
            },
        .transient_attachments = true,
        .dynamic_rendering_local_read = feature,
    };
}

} // namespace

TEST_CASE("rendering sample planning discovers a hybrid attachment component",
          "[sample-count][rendering]") {
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment |
                   ImageUsageBit::transfer_src),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto query = [](const RenderTargetDefinition &definition) {
        if (definition.name == "custom_id") {
            return std::vector<std::uint32_t>{1, 2};
        }
        return std::vector<std::uint32_t>{1, 2, 4};
    };

    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const auto compilation = compileRenderingTargetPlans(
        withImagesOf(graphs, targets), targets, compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm,
        RenderingTargetPlanDeviceFacts{
            .query_attachment_samples = query,
        });
    REQUIRE(assignment(compilation, "albedo").samples == 2);
    REQUIRE(assignment(compilation, "normal").samples == 2);
    REQUIRE(assignment(compilation, "custom_id").samples == 2);
    REQUIRE(assignment(compilation, "depth").samples == 2);
    REQUIRE(assignment(compilation, "lit").samples == 2);
    REQUIRE(assignment(compilation, "display").samples == 1);
    REQUIRE(compilation.plans.size() == 1);
    REQUIRE(compilation.plans.front()
                ->sample_count_plan->groups.at(0)
                .limiting_resources ==
            std::vector<std::string>{"custom_id (R32_UINT)"});
    REQUIRE(physicalResource(
                *compilation.plans.front(), "custom_id")
                .rasterization_samples == 2);
    REQUIRE(
        compilation.plans.front()
            ->attachments.size() == 8);
    REQUIRE(
        physicalAttachment(
            *compilation.plans.front(),
            "geometry", "albedo")
            .operations.load_op ==
        AttachmentLoadOp::clear);
    REQUIRE(
        physicalAttachment(
            *compilation.plans.front(),
            "geometry", "depth")
            .operations.store_op ==
        AttachmentStoreOp::dont_care);
    REQUIRE(
        ejectVulkanPhysicalFragmentPackage(
            *compilation.plans.front())
            .schema_version == 3);
}

TEST_CASE("ray query target requirement is hard and uses the enabled runtime capability",
          "[target-planning][ray-query][capability]") {
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment |
                   ImageUsageBit::transfer_src),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const auto query = [](const RenderTargetDefinition &) {
        return std::vector<std::uint32_t>{1, 2, 4};
    };
    const TargetPlanningPolicy planning{
        .graphs = {
            PlanningGraphConstraints{
                .graph = "main",
                .required_capabilities = {
                    std::string{vulkanRayQueryCapability}},
            },
        },
        .authored = true,
    };
    const auto compile = [&](bool ray_query) {
        return compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .ray_query = ray_query,
                .query_attachment_samples = query,
            },
            std::nullopt, std::nullopt, planning);
    };

    requireThrowsContaining(
        [&] { (void)compile(false); },
        "pelican.plan.ray_query_required_unavailable@1");

    const auto capable = compile(true);
    REQUIRE(capable.plans.size() == 1);
    REQUIRE(std::find(
                capable.plans.front()
                    ->required_physical_features.begin(),
                capable.plans.front()
                    ->required_physical_features.end(),
                vulkanRayQueryCapability) !=
            capable.plans.front()
                ->required_physical_features.end());
}

TEST_CASE(
    "ray tracing pipeline target requirement is hard and uses the enabled runtime capability",
    "[target-planning][ray-tracing][capability][wp285]") {
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment |
                   ImageUsageBit::transfer_src),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const auto query = [](const RenderTargetDefinition &) {
        return std::vector<std::uint32_t>{1, 2, 4};
    };
    const TargetPlanningPolicy planning{
        .graphs = {
            PlanningGraphConstraints{
                .graph = "main",
                .required_capabilities = {
                    std::string{
                        vulkanRayTracingPipelineCapability}},
            },
        },
        .authored = true,
    };
    const auto compile = [&](bool ray_tracing_pipeline) {
        return compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .ray_tracing_pipeline = ray_tracing_pipeline,
                .query_attachment_samples = query,
            },
            std::nullopt, std::nullopt, planning);
    };

    requireThrowsContaining(
        [&] { (void)compile(false); },
        "pelican.plan.ray_tracing_pipeline_required_unavailable@1");

    const auto capable = compile(true);
    REQUIRE(capable.plans.size() == 1);
    REQUIRE(std::find(
                capable.plans.front()
                    ->required_physical_features.begin(),
                capable.plans.front()
                    ->required_physical_features.end(),
                vulkanRayTracingPipelineCapability) !=
            capable.plans.front()
                ->required_physical_features.end());
}

TEST_CASE("rendering target bridge links attachment operations into the physical plan",
          "[target-planning][rendering][physical-attachment][bridge]") {
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment |
                   ImageUsageBit::transfer_src),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(
            config);
    const auto facts =
        RenderingTargetPlanDeviceFacts{
            .query_attachment_samples =
                [](const RenderTargetDefinition &) {
                    return std::vector<std::uint32_t>{
                        1, 2, 4};
                },
        };
    const auto automatic =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            facts);
    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            *automatic.plans.front());
    const auto attachment = std::find_if(
        fragment.attachments->begin(),
        fragment.attachments->end(),
        [](const auto &candidate) {
            return candidate.node ==
                       "geometry" &&
                   candidate.logical_resource ==
                       "albedo";
        });
    REQUIRE(
        attachment !=
        fragment.attachments->end());
    attachment->load_op =
        AttachmentLoadOp::dont_care;

    const std::array fragments{fragment};
    const auto linked =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            facts,
            std::nullopt, std::nullopt, {},
            {}, fragments);
    REQUIRE(
        physicalAttachment(
            *linked.plans.front(),
            "geometry", "albedo")
            .operations.load_op ==
        AttachmentLoadOp::dont_care);
    REQUIRE(
        linked.plans.front()
            ->applied_fragment_package ==
        std::optional<
            VulkanPhysicalFragmentPackage>{
            fragment});
    REQUIRE(linked.verification_contexts.size() == 1);
    const auto &verification =
        linked.verification_contexts.front();
    REQUIRE(verification.logical_graph != nullptr);
    REQUIRE(verification.automatic_plan != nullptr);
    REQUIRE(verification.compiled_plan ==
            linked.plans.front());
    REQUIRE(verification.automatic_plan !=
            verification.compiled_plan);
    REQUIRE_FALSE(
        verification.automatic_plan
            ->applied_fragment_package.has_value());
    REQUIRE(
        verification.automatic_plan
            ->automatic_plan_fingerprint ==
        vulkanAutomaticTargetPlanFingerprint(
            verification.topology,
            *verification.automatic_plan));
    REQUIRE(
        verification.compiled_plan
            ->automatic_plan_fingerprint ==
        verification.automatic_plan
            ->automatic_plan_fingerprint);
}

TEST_CASE(
    "runtime target bridge preserves cube shape through device capability "
    "planning",
    "[target-planning][rendering][cube][wp236]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "render_targets": [{
            "name": "environment_probe",
            "extent_scale": 1.0,
            "width": 128,
            "height": 128,
            "dimension": "cube",
            "format": "R8G8B8A8_UNORM",
            "usage": ["COLOR_ATTACHMENT", "SAMPLED"],
            "mip_levels": "full"
          }],
          "rendering_passes": [{
            "name": "cube_capture",
            "passes": [{
              "name": "capture_face",
              "type": "fullscreen",
              "output": {
                "color": [{
                  "target": "environment_probe",
                  "subresource": {
                    "mip": 0,
                    "layer": 2
                  }
                }],
                "depth": null
              }
            }]
          }]
        })json");
    const auto graphs =
        TestSupport::renderGraphOf(
            config);
    const auto targets =
        Pelican::test_support::renderTargetDefinitionsOf(
            config);
    bool queried_cube = false;
    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_image_format_capability =
                    [&](const RenderTargetDefinition
                            &definition) {
                        queried_cube =
                            definition.dimension ==
                                ImageResourceDimension::
                                    cube &&
                            definition.array_layers == 6;
                        return RenderingImageFormatCapability{
                            .image_usage_supported =
                                true,
                            .supported_samples = {1},
                            .max_mip_levels = 8,
                            .max_array_layers = 6,
                        };
                    },
            });
    REQUIRE(queried_cube);
    REQUIRE(compilation.plans.size() == 1);
    const auto &physical =
        physicalResource(
            *compilation.plans.front(),
            "environment_probe");
    REQUIRE(
        physical.dimension ==
        ImageResourceDimension::cube);
    REQUIRE(physical.array_layers == 6);
    REQUIRE(
        physical.mip_levels.mode ==
        ImageMipLevelMode::full_chain);

    auto applied = targets;
    applyPlan(
        applied, compilation);
    REQUIRE(applied.size() == 1);
    REQUIRE(
        applied.front().dimension ==
        ImageResourceDimension::cube);
    REQUIRE(applied.front().array_layers == 6);
}

TEST_CASE(
    "runtime target adapter executes supported write-only attachments as transient",
    "[target-planning][rendering][transient-attachment][runtime-adapter]") {
    const auto config =
        writeOnlyAttachmentConfig();
    const auto graphs =
        TestSupport::renderGraphOf(
            config);
    auto scratch =
        target(
            "scratch",
            ImageFormat::r8g8b8a8_unorm,
            ImageUsageBit::
                color_attachment);
    scratch.format_candidates = {
        ImageFormat::r16g16b16a16_sfloat};
    const std::vector targets{scratch};
    const auto supported_facts =
        RenderingTargetPlanDeviceFacts{
            .query_image_format_capability =
                [](const RenderTargetDefinition &) {
                    return RenderingImageFormatCapability{
                        .image_usage_supported = true,
                        .supported_samples = {1},
                        .max_array_layers = 1,
                        .transient_attachment_supported =
                            true,
                    };
                },
            .transient_attachments = true,
        };

    const auto transient =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            supported_facts);
    REQUIRE(transient.plans.size() == 1);
    const auto &plan = *transient.plans.front();
    REQUIRE(
        plan.backend_selection
            .selected_candidate ==
        "pelican.vulkan.transient_plan@1");
    REQUIRE(
        physicalResource(plan, "scratch")
            .representation ==
        VulkanResourceRepresentation::
            transient_attachment);
    REQUIRE(
        physicalAttachment(
            plan, "transient_probe", "scratch")
            .operations.store_op ==
        AttachmentStoreOp::dont_care);
    REQUIRE(
        representationAssignment(
            transient, "scratch")
            .representation ==
        VulkanResourceRepresentation::
            transient_attachment);

    auto applied_targets = targets;
    applyPlan(
        applied_targets, transient);
    REQUIRE(
        applied_targets.front().storage_mode ==
        RenderTargetStorageMode::
            transient_attachment);

    auto unsupported_facts = supported_facts;
    unsupported_facts
        .query_image_format_capability =
        [](const RenderTargetDefinition &definition) {
            return RenderingImageFormatCapability{
                .image_usage_supported = true,
                .supported_samples = {1},
                .max_array_layers = 1,
                .transient_attachment_supported =
                    definition.format ==
                    ImageFormat::r16g16b16a16_sfloat,
            };
        };
    const auto unsupported =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            unsupported_facts);
    REQUIRE(
        unsupported.plans.front()
            ->backend_selection
            .selected_candidate ==
        "pelican.vulkan.materialized_plan@1");
    REQUIRE(
        representationAssignment(
            unsupported, "scratch")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);

    const auto conservative =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            supported_facts,
            std::nullopt, std::nullopt,
            TargetPlanningPolicy{
                .profile = {
                    PlanningProfileKind::
                        conservative_debug,
                    0},
                .authored = true,
            });
    REQUIRE(
        conservative.plans.front()
            ->backend_selection
            .selected_candidate ==
        "pelican.vulkan.materialized_plan@1");
    REQUIRE(
        physicalAttachment(
            *conservative.plans.front(),
            "transient_probe", "scratch")
            .operations.store_op ==
        AttachmentStoreOp::store);

    auto alternate_fragment =
        ejectVulkanPhysicalFragmentPackage(
            plan);
    const auto fragment_resource =
        std::find_if(
            alternate_fragment.resources.begin(),
            alternate_fragment.resources.end(),
            [](const auto &resource) {
                return resource.logical_resource ==
                       "scratch";
            });
    REQUIRE(
        fragment_resource !=
        alternate_fragment.resources.end());
    fragment_resource->format =
        ImageFormat::r16g16b16a16_sfloat;
    const std::array fragments{
        alternate_fragment};
    auto format_sensitive_facts =
        supported_facts;
    format_sensitive_facts
        .query_image_format_capability =
        [](const RenderTargetDefinition &definition) {
            return RenderingImageFormatCapability{
                .image_usage_supported = true,
                .supported_samples = {1},
                .max_array_layers = 1,
                .transient_attachment_supported =
                    definition.format ==
                    ImageFormat::r8g8b8a8_unorm,
            };
        };
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets,
                compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                format_sensitive_facts,
                std::nullopt, std::nullopt, {},
                {}, fragments);
        },
        "alternate format requires a materialized_image");
}

TEST_CASE(
    "runtime target adapter selects supported fullscreen local reads and materializes every fallback",
    "[target-planning][rendering][tile-local][runtime-adapter]") {
    const auto config =
        localReadAttachmentConfig();
    const auto graphs =
        TestSupport::renderGraphOf(
            config);
    const auto targets =
        Pelican::test_support::renderTargetDefinitionsOf(
            config);

    const auto local =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            localReadDeviceFacts());
    REQUIRE(local.plans.size() == 1);
    const auto &plan = *local.plans.front();
    REQUIRE(
        plan.backend_selection
            .selected_candidate ==
        "pelican.vulkan.tile_local_plan@1");
    REQUIRE(
        physicalResource(plan, "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            tile_local_attachment);
    REQUIRE(
        physicalResource(plan, "lit")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);
    REQUIRE(std::any_of(
        plan.scopes.begin(), plan.scopes.end(),
        [](const auto &scope) {
            return scope.nodes ==
                       std::vector<std::string>{
                           "geometry", "lighting"} &&
                   scope.local_reads ==
                       std::vector<std::string>{
                           "gbuffer"};
        }));
    REQUIRE(
        physicalAttachment(
            plan, "geometry", "gbuffer")
            .operations.store_op ==
        AttachmentStoreOp::
            dont_care);

    auto applied = targets;
    applyPlan(applied, local);
    const auto gbuffer = std::find_if(
        applied.begin(), applied.end(),
        [](const auto &target) {
            return target.name == "gbuffer";
        });
    REQUIRE(gbuffer != applied.end());
    REQUIRE(
        gbuffer->storage_mode ==
        RenderTargetStorageMode::
            tile_local_attachment);
    REQUIRE(
        gbuffer->usage &
        ImageUsageBit::
            input_attachment);

    const auto missing_feature =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            localReadDeviceFacts(false, true));
    REQUIRE(
        representationAssignment(
            missing_feature, "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);

    const auto missing_format =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            localReadDeviceFacts(true, false));
    REQUIRE(
        representationAssignment(
            missing_format, "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);
}

TEST_CASE(
    "runtime local-read eligibility accepts material consumers and rejects incompatible extents and multisampling",
    "[target-planning][rendering][tile-local][fallback]") {
    const auto compile_config =
        [](const nlohmann::json &config,
           SampleCountPolicy samples = {}) {
            const auto graphs =
                TestSupport::renderGraphOf(
                    config);
            const auto targets =
                Pelican::test_support::renderTargetDefinitionsOf(
                    config);
            return compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets, samples,
                vk::Format::eB8G8R8A8Unorm,
                localReadDeviceFacts());
        };

    const auto material_consumer =
        compile_config(
            localReadAttachmentConfig("material"));
    REQUIRE(
        material_consumer.plans.front()
            ->backend_selection
            .selected_candidate ==
        "pelican.vulkan.tile_local_plan@1");
    REQUIRE(
        representationAssignment(
            material_consumer, "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            tile_local_attachment);

    const auto unsupported_output_consumer =
        compile_config(
            localReadAttachmentConfig(
                "output_transform"));
    REQUIRE(
        representationAssignment(
            unsupported_output_consumer,
            "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);

    const auto mismatched_extent =
        compile_config(
            localReadAttachmentConfig(
                "fullscreen", 0.5f));
    REQUIRE(
        mismatched_extent.plans.front()
            ->backend_selection
            .selected_candidate ==
        "pelican.vulkan.materialized_plan@1");
    REQUIRE(
        representationAssignment(
            mismatched_extent, "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);

    auto multisampled_config =
        localReadAttachmentConfig();
    multisampled_config["multisampling"] = {
        {"fallback", "error"},
        {"samples", 4},
        {"scope", "none"},
        {"targets",
         nlohmann::json::array({"gbuffer"})},
    };
    const auto multisampled =
        compile_config(
            multisampled_config,
            compileSampleCountPolicy(
                multisampled_config));
    REQUIRE(
        assignment(multisampled, "gbuffer")
            .samples == 4);
    REQUIRE(
        representationAssignment(
            multisampled, "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);
}

TEST_CASE(
    "XR multiview planning preserves a fused tile-local attachment scope",
    "[wp203][wp204][target-planning][multiview][tile-local][xr]") {
    const auto config =
        localReadAttachmentConfig();
    const auto graphs =
        TestSupport::renderGraphOf(
            config);
    const auto targets =
        Pelican::test_support::renderTargetDefinitionsOf(
            config);
    auto device_facts = localReadDeviceFacts();
    device_facts.multiview = true;
    device_facts.max_multiview_view_count = 2;

    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            std::move(device_facts),
            VulkanViewExecutionPlanRequest{
                .view_count = 2,
                .preference =
                    XrViewExecutionPreference::
                        require_multiview,
                .multiview_capable_nodes =
                    {"geometry", "lighting",
                     "present"},
            });

    REQUIRE(compilation.plans.size() == 1);
    const auto &plan = *compilation.plans.front();
    REQUIRE(
        plan.backend_selection
            .selected_candidate ==
        "pelican.vulkan.tile_local_plan@1");
    REQUIRE(
        plan.view_execution_plan.uses_multiview);
    REQUIRE(
        plan.view_execution_plan.view_count == 2);

    const auto fused_scope = std::find_if(
        plan.scopes.begin(), plan.scopes.end(),
        [](const auto &scope) {
            return scope.nodes ==
                   std::vector<std::string>{
                       "geometry", "lighting"};
        });
    REQUIRE(fused_scope != plan.scopes.end());
    REQUIRE(
        fused_scope->local_reads ==
        std::vector<std::string>{"gbuffer"});
    REQUIRE(
        fused_scope->view_execution ==
        VulkanScopeViewExecution::multiview);
    REQUIRE(fused_scope->view_count == 2);
    REQUIRE(fused_scope->execution_count == 1);
    REQUIRE(fused_scope->view_mask == 0b11);

    const auto &gbuffer =
        physicalResource(plan, "gbuffer");
    REQUIRE(
        gbuffer.representation ==
        VulkanResourceRepresentation::
            tile_local_attachment);
    REQUIRE(
        gbuffer.view_layout ==
        VulkanResourceViewLayout::
            layered_2d_array);
    REQUIRE(gbuffer.array_layers == 2);
    REQUIRE(
        layerAssignment(compilation, "gbuffer")
            .array_layers == 2);

    auto applied = targets;
    applyPlan(applied, compilation);
    const auto target = std::find_if(
        applied.begin(), applied.end(),
        [](const auto &candidate) {
            return candidate.name == "gbuffer";
        });
    REQUIRE(target != applied.end());
    REQUIRE(
        target->storage_mode ==
        RenderTargetStorageMode::
            tile_local_attachment);
    REQUIRE(target->array_layers == 2);
    REQUIRE(
        target->usage &
        ImageUsageBit::input_attachment);
}

TEST_CASE(
    "runtime target bridge materializes a shared target without losing local-read usage",
    "[target-planning][rendering][tile-local][multi-graph]") {
    auto config = localReadAttachmentConfig();
    auto material_graph =
        localReadAttachmentConfig("material")
            ["rendering_passes"]
            .front();
    material_graph["name"] =
        "material_fallback";
    material_graph["passes"][1]
                  ["material_resources"]
                  ["gbuffer_input"]
                  ["footprint"] =
        "neighborhood";
    config["rendering_passes"].push_back(
        std::move(material_graph));

    const auto graphs =
        TestSupport::renderGraphOf(
            config);
    const auto targets =
        Pelican::test_support::renderTargetDefinitionsOf(
            config);
    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            localReadDeviceFacts());

    REQUIRE(compilation.plans.size() == 2);
    REQUIRE(
        compilation.plans[0]
            ->backend_selection
            .selected_candidate ==
        "pelican.vulkan.tile_local_plan@1");
    REQUIRE(
        compilation.plans[1]
            ->backend_selection
            .selected_candidate ==
        "pelican.vulkan.materialized_plan@1");
    REQUIRE(
        representationAssignment(
            compilation, "gbuffer")
            .representation ==
        VulkanResourceRepresentation::
            materialized_image);

    auto applied = targets;
    applyPlan(
        applied, compilation);
    const auto gbuffer = std::find_if(
        applied.begin(), applied.end(),
        [](const auto &target) {
            return target.name == "gbuffer";
        });
    REQUIRE(gbuffer != applied.end());
    REQUIRE(
        gbuffer->storage_mode ==
        RenderTargetStorageMode::materialized);
    REQUIRE(
        gbuffer->usage &
        ImageUsageBit::
            input_attachment);
    REQUIRE(
        gbuffer->usage &
        ImageUsageBit::sampled);
}

TEST_CASE("rendering target bridge applies a verified alternate physical format",
          "[target-planning][rendering][physical-format][bridge]") {
    auto targets = std::vector{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment |
                   ImageUsageBit::transfer_src),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto lit = std::find_if(
        targets.begin(), targets.end(),
        [](const auto &candidate) {
            return candidate.name == "lit";
        });
    REQUIRE(lit != targets.end());
    lit->format_candidates = {
        ImageFormat::r16g16b16a16_sfloat,
        ImageFormat::r8g8b8a8_unorm,
    };

    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(
            config);
    const auto facts =
        RenderingTargetPlanDeviceFacts{
            .query_image_format_capability =
                [](const RenderTargetDefinition &) {
                    return RenderingImageFormatCapability{
                        .image_usage_supported = true,
                        .supported_samples = {1, 2, 4},
                        .max_array_layers = 4,
                    };
                },
        };
    const auto automatic =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            facts);
    REQUIRE(
        formatAssignment(automatic, "lit").format ==
        ImageFormat::r16g16b16a16_sfloat);

    auto fragment =
        ejectVulkanPhysicalFragmentPackage(
            *automatic.plans.front());
    const auto resource = std::find_if(
        fragment.resources.begin(),
        fragment.resources.end(),
        [](const auto &candidate) {
            return candidate.logical_resource ==
                   "lit";
        });
    REQUIRE(resource != fragment.resources.end());
    resource->format =
        ImageFormat::r8g8b8a8_unorm;
    const std::array fragments{fragment};
    const auto linked =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            facts,
            std::nullopt, std::nullopt, {},
            {}, fragments);
    REQUIRE(
        formatAssignment(linked, "lit").format ==
        ImageFormat::r8g8b8a8_unorm);
    REQUIRE(
        physicalResource(
            *linked.plans.front(), "lit")
            .format ==
        ImageFormat::r8g8b8a8_unorm);
    REQUIRE(
        std::any_of(
            linked.plans.front()
                ->sample_count_plan->resources.begin(),
            linked.plans.front()
                ->sample_count_plan->resources.end(),
            [](const auto &candidate) {
                return candidate.resource == "lit" &&
                       candidate.format ==
                           ImageFormat::r8g8b8a8_unorm;
            }));

    auto applied_targets = targets;
    applyPlan(
        applied_targets, linked);
    const auto applied_lit = std::find_if(
        applied_targets.begin(),
        applied_targets.end(),
        [](const auto &candidate) {
            return candidate.name == "lit";
        });
    REQUIRE(applied_lit !=
            applied_targets.end());
    REQUIRE(applied_lit->format ==
            ImageFormat::r8g8b8a8_unorm);

    auto unsupported_facts = facts;
    unsupported_facts
        .query_image_format_capability =
        [](const RenderTargetDefinition &definition) {
            return RenderingImageFormatCapability{
                .image_usage_supported =
                    definition.format !=
                    ImageFormat::r8g8b8a8_unorm,
                .supported_samples = {1, 2, 4},
                .max_array_layers = 4,
            };
        };
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets,
                compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                unsupported_facts,
                std::nullopt, std::nullopt, {},
                {}, fragments);
        },
        "does not support the required image usage");
}

TEST_CASE("rendering target bridge applies typed graph planning controls",
          "[target-planning][rendering][bridge]") {
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment |
                   ImageUsageBit::transfer_src),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const auto query = [](const RenderTargetDefinition &) {
        return std::vector<std::uint32_t>{1, 2, 4};
    };
    const TargetPlanningPolicy planning{
        .profile = {
            PlanningProfileKind::hazard_stress, 23},
        .graphs = {
            PlanningGraphConstraints{
                .graph = "main",
                .nodes = {
                    PlanningNodeConstraint{
                        .node = "geometry",
                        .serial = true,
                    },
                },
                .resources = {
                    PlanningResourceConstraint{
                        .resource = "depth",
                        .no_alias = true,
                    },
                },
            },
        },
        .authored = true,
    };

    const auto compilation = compileRenderingTargetPlans(
        withImagesOf(graphs, targets), targets, compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm,
        RenderingTargetPlanDeviceFacts{
            .query_attachment_samples = query,
        },
        std::nullopt, std::nullopt, planning);
    REQUIRE(compilation.plans.size() == 1);
    const auto &plan = *compilation.plans.front();
    REQUIRE((plan.opportunities.profile ==
             PlanningProfile{
                 PlanningProfileKind::hazard_stress, 23}));
    REQUIRE(std::none_of(
        plan.opportunities.parallel_candidates.begin(),
        plan.opportunities.parallel_candidates.end(),
        [](const PlanningNamePair &pair) {
            return pair.first == "geometry" ||
                   pair.second == "geometry";
        }));
    REQUIRE(std::none_of(
        plan.alias_groups.begin(), plan.alias_groups.end(),
        [](const VulkanAliasGroupPlan &group) {
            return std::find(
                       group.resources.begin(),
                       group.resources.end(),
                       "depth") != group.resources.end();
        }));

    const std::vector pins{
        ejectVulkanTargetPlanPinPackage(plan)};
    const auto pinned_compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples = query,
            },
            std::nullopt, std::nullopt, planning,
            pins);
    REQUIRE(
        pinned_compilation.plans.front()
            ->applied_pin_package ==
        std::optional<VulkanTargetPlanPinPackage>{
            pins.front()});

    const std::vector physical_fragments{
        ejectVulkanPhysicalFragmentPackage(plan)};
    const auto fragment_compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples = query,
            },
            std::nullopt, std::nullopt, planning,
            {}, physical_fragments);
    REQUIRE(
        fragment_compilation.plans.front()
            ->applied_fragment_package ==
        std::optional<VulkanPhysicalFragmentPackage>{
            physical_fragments.front()});
    REQUIRE(
        fragment_compilation.plans.front()
            ->resources == plan.resources);
    REQUIRE(
        fragment_compilation.plans.front()
            ->scopes == plan.scopes);

    auto stale_fragments = physical_fragments;
    stale_fragments.front()
        .automatic_plan_fingerprint ^= 1;
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets,
                compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                RenderingTargetPlanDeviceFacts{
                    .query_attachment_samples = query,
                },
                std::nullopt, std::nullopt, planning,
                {}, stale_fragments);
        },
        "stale for the current target facts/provider generation");

    auto stale_pins = pins;
    stale_pins.front().logical_graph_fingerprint ^= 1;
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets,
                compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                RenderingTargetPlanDeviceFacts{
                    .query_attachment_samples = query,
                },
                std::nullopt, std::nullopt, planning,
                stale_pins);
        },
        "pin package is stale");

    auto unknown_fragments = physical_fragments;
    unknown_fragments.front().graph = "missing";
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets,
                compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                RenderingTargetPlanDeviceFacts{
                    .query_attachment_samples = query,
                },
                std::nullopt, std::nullopt, planning,
                {}, unknown_fragments);
        },
        "physical fragments reference unknown graph: missing");

    auto unknown_graph = planning;
    unknown_graph.graphs.front().graph = "missing";
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets, compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                RenderingTargetPlanDeviceFacts{
                    .query_attachment_samples = query,
                },
                std::nullopt, std::nullopt,
                unknown_graph);
        },
        "constraints reference unknown graph: missing");

    auto unknown_node = planning;
    unknown_node.graphs.front().nodes.front().node =
        "missing";
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets, compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                RenderingTargetPlanDeviceFacts{
                    .query_attachment_samples = query,
                },
                std::nullopt, std::nullopt,
                unknown_node);
        },
        "constraint references unknown node: missing");
}

TEST_CASE("exact rendering sample planning reports an added G-buffer target",
          "[sample-count][rendering]") {
    auto config = hybridConfig();
    config["multisampling"]["fallback"] = "error";
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    requireThrowsContaining(
        [&] {
            const auto graphs =
                TestSupport::renderGraphOf(config);
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets,
                compileSampleCountPolicy(config),
                vk::Format::eB8G8R8A8Unorm,
                RenderingTargetPlanDeviceFacts{
                    .query_attachment_samples =
                        [](const RenderTargetDefinition &definition) {
                            return definition.name == "custom_id"
                                       ? std::vector<std::uint32_t>{1}
                                       : std::vector<std::uint32_t>{
                                             1, 2, 4};
                        },
                });
        },
        "custom_id (R32_UINT)");
}

TEST_CASE("explicit target selects its whole attachment component",
          "[sample-count][rendering]") {
    auto config = hybridConfig();
    config["multisampling"]["scope"] = "none";
    config["multisampling"]["targets"] = nlohmann::json::array({"lit"});
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const auto compilation = compileRenderingTargetPlans(
        withImagesOf(graphs, targets), targets, compileSampleCountPolicy(config),
        vk::Format::eB8G8R8A8Unorm,
        RenderingTargetPlanDeviceFacts{
            .query_attachment_samples =
                [](const auto &) {
                    return std::vector<std::uint32_t>{1, 2, 4};
                },
        });
    REQUIRE(assignment(compilation, "albedo").samples == 4);
    REQUIRE(assignment(compilation, "lit").samples == 4);
    REQUIRE(assignment(compilation, "display").samples == 1);
}

TEST_CASE("color resolve mode follows Vulkan numeric format rules",
          "[sample-count][rendering][resolve]") {
    REQUIRE(colorAttachmentResolveMode(ImageFormat::r32_uint) ==
            vk::ResolveModeFlagBits::eSampleZero);
    REQUIRE(colorAttachmentResolveMode(ImageFormat::r16_sint) ==
            vk::ResolveModeFlagBits::eSampleZero);
    REQUIRE(colorAttachmentResolveMode(
                ImageFormat::r16g16b16a16_sfloat) ==
            vk::ResolveModeFlagBits::eAverage);
    REQUIRE(colorAttachmentResolveMode(
                ImageFormat::b8g8r8a8_unorm) ==
            vk::ResolveModeFlagBits::eAverage);
}

TEST_CASE(
    "runtime target bridge applies compatible lifetime alias groups",
    "[target-planning][alias][runtime]") {
    auto display =
        target("display",
               ImageFormat::r8g8b8a8_unorm);
    display.format_class = "display";
    std::vector targets{
        target("temporary_a",
               ImageFormat::r8g8b8a8_unorm),
        target("temporary_b",
               ImageFormat::r8g8b8a8_unorm),
        display,
    };
    const auto graphs =
        TestSupport::renderGraphOf(
            aliasLifetimeConfig());
    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<
                            std::uint32_t>{1};
                    },
            });

    REQUIRE(compilation.plans.size() == 1);
    REQUIRE(
        compilation.plans.front()
            ->alias_groups.size() == 1);
    REQUIRE(
        compilation.plans.front()
            ->alias_groups.front()
            .resources ==
        std::vector<std::string>{
            "temporary_a", "temporary_b"});
    REQUIRE(
        compilation.alias_group_assignments.size() ==
        1);
    REQUIRE(
        compilation.alias_group_assignments.front()
            .resources ==
        std::vector<std::string>{
            "temporary_a", "temporary_b"});

    auto duplicate_group = compilation;
    duplicate_group.alias_group_assignments.push_back(
        duplicate_group.alias_group_assignments.front());
    auto duplicate_targets = targets;
    REQUIRE_THROWS_WITH(
        applyPlan(
            duplicate_targets, duplicate_group),
        Catch::Matchers::ContainsSubstring(
            "duplicate physical alias group assignment id"));

    applyPlan(targets, compilation);
    REQUIRE(targets[0].alias_group.has_value());
    REQUIRE(targets[1].alias_group ==
            targets[0].alias_group);
    REQUIRE_FALSE(targets[2].alias_group.has_value());
}

TEST_CASE(
    "PO-1 a target kept out of runtime aliasing by its usage names the usage in the plan's reason",
    "[target-planning][alias][runtime][po1]") {
    auto display =
        target("display",
               ImageFormat::r8g8b8a8_unorm);
    display.format_class = "display";
    const auto compile = [&](ImageUsage temporary_b_usage) {
        const std::vector targets{
            target("temporary_a",
                   ImageFormat::r8g8b8a8_unorm),
            target("temporary_b",
                   ImageFormat::r8g8b8a8_unorm,
                   temporary_b_usage),
            display,
        };
        return compileRenderingTargetPlans(
            withImagesOf(TestSupport::renderGraphOf(
                             aliasLifetimeConfig()),
                         targets),
            targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<
                            std::uint32_t>{1};
                    },
            });
    };
    const auto representation_detail =
        [](const VulkanTargetPlan &plan, std::string_view resource) {
            const auto found = std::find_if(
                plan.decisions.begin(), plan.decisions.end(),
                [&](const auto &decision) {
                    return decision.id ==
                               "pelican.plan.resource_representation@1" &&
                           decision.subject == resource;
                });
            REQUIRE(found != plan.decisions.end());
            return found->detail;
        };

    const auto transfer =
        compile(ImageUsageBit::color_attachment |
                ImageUsageBit::sampled |
                ImageUsageBit::transfer_src);
    const auto &excluded = *transfer.plans.front();
    REQUIRE(excluded.alias_groups.empty());
    const std::string expected =
        "logical materialization requirement is required: usage "
        "TRANSFER_SRC is outside the alias-safe set "
        "{COLOR_ATTACHMENT, SAMPLED}";
    CHECK(physicalResource(excluded, "temporary_b").reason ==
          expected);
    CHECK(representation_detail(excluded, "temporary_b") ==
          expected);
    // A candidate's reason is as before.
    CHECK(physicalResource(excluded, "temporary_a").reason ==
          "arbitrary read requires a materialized resource");
    CHECK(physicalResource(excluded, "display").reason ==
          "logical materialization requirement is required: "
          "display-format target is not aliased");

    // Without TRANSFER_SRC it is a candidate again (the rule is unchanged).
    const auto sampled =
        compile(ImageUsageBit::color_attachment |
                ImageUsageBit::sampled);
    REQUIRE(sampled.plans.front()->alias_groups.size() == 1);
    CHECK(physicalResource(*sampled.plans.front(), "temporary_b")
              .reason ==
          "arbitrary read requires a materialized resource");

    // Every usage past the set is named, in the usage table's order.
    const auto storage =
        compile(ImageUsageBit::color_attachment |
                ImageUsageBit::sampled |
                ImageUsageBit::transfer_src |
                ImageUsageBit::storage);
    CHECK(physicalResource(*storage.plans.front(), "temporary_b")
              .reason ==
          "logical materialization requirement is required: usage "
          "STORAGE, TRANSFER_SRC are outside the alias-safe set "
          "{COLOR_ATTACHMENT, SAMPLED}");
}

TEST_CASE(
    "runtime alias merge drops groups that graph variants do not share",
    "[target-planning][alias][runtime][variant]") {
    auto display =
        target("display",
               ImageFormat::r8g8b8a8_unorm);
    display.format_class = "display";
    const std::vector targets{
        target("temporary_a",
               ImageFormat::r8g8b8a8_unorm),
        target("temporary_b",
               ImageFormat::r8g8b8a8_unorm),
        display,
    };
    auto alias_graphs =
        TestSupport::renderGraphOf(
            aliasLifetimeConfig());
    auto single_graphs =
        TestSupport::renderGraphOf(
            aliasLifetimeConfig(false));
    // RC-3c1b: the two variants' graphs in one IR.
    alias_graphs.graphs.insert(
        alias_graphs.graphs.end(),
        single_graphs.graphs.begin(),
        single_graphs.graphs.end());

    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(alias_graphs, targets), targets,
            SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<
                            std::uint32_t>{1};
                    },
            });
    REQUIRE(
        compilation.plans.front()
            ->alias_groups.size() == 1);
    REQUIRE(
        compilation.alias_group_assignments.empty());
}

TEST_CASE(
    "RC-1 when graph variants do not share an alias group, the memory total counts its "
    "targets as two allocations",
    "[target-planning][alias][runtime][variant][rc1][memory]") {
    // The input above: the first graph alone could let temporary_a and
    // temporary_b take turns in one allocation; the second reads
    // temporary_a without temporary_b.
    auto display =
        target("display",
               ImageFormat::r8g8b8a8_unorm);
    display.format_class = "display";
    std::vector targets{
        target("temporary_a",
               ImageFormat::r8g8b8a8_unorm),
        target("temporary_b",
               ImageFormat::r8g8b8a8_unorm),
        display,
    };
    auto graphs =
        TestSupport::renderGraphOf(
            aliasLifetimeConfig());
    const auto single_graphs =
        TestSupport::renderGraphOf(
            aliasLifetimeConfig(false));
    graphs.graphs.insert(graphs.graphs.end(), single_graphs.graphs.begin(),
                         single_graphs.graphs.end());
    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<
                            std::uint32_t>{1};
                    },
            });
    REQUIRE(compilation.plans.size() == 2);
    REQUIRE(compilation.plans.front()->alias_groups.size() == 1);
    REQUIRE(compilation.alias_group_assignments.empty());

    // Registration allocates what the runtime kept: three images of their
    // own (64x64 R8G8B8A8, 16,384 bytes each), not two.
    applyPlan(targets, compilation);
    const auto registered =
        test_support::registerRenderTargetsWithoutDevice(
            targets, {64, 64});
    const auto memory = test_support::registeredTargetMemoryOf(
        registered, [](std::string_view) { return true; });
    for (const auto &entry : memory) {
        INFO(entry.name);
        CHECK(entry.bytes == 16'384);
        CHECK_FALSE(entry.alias_group.has_value());
    }
    const auto total = totalRegisteredTargetMemory(memory);
    CHECK(total.total_bytes == 3 * 16'384);
    CHECK(total.unused_bytes == 0);
}

TEST_CASE(
    "runtime target plan carries low-resolution scene and output extent contracts",
    "[target-planning][upscale][resolution]") {
    const auto config = nlohmann::json{
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "upscale"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "produce_low"},
                      {"type", "fullscreen"},
                      {"resolution_domain", "scene"},
                      {"output",
                       {{"color", "low_color"},
                        {"depth", nullptr}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"input",
                       nlohmann::json::array(
                           {"low_color"})},
                      {"output",
                       {{"color", "display"},
                        {"depth", nullptr}}}}})}}})},
    };
    auto low =
        target("low_color",
               ImageFormat::r8g8b8a8_unorm);
    low.extent_scale = 0.5f;
    auto display =
        target("display",
               ImageFormat::b8g8r8a8_srgb);
    const std::vector targets{low, display};
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<std::uint32_t>{1};
                    },
            });

    REQUIRE(compilation.plans.size() == 1);
    const auto &plan = *compilation.plans.front();
    REQUIRE(plan.resolution_plan.has_value());
    REQUIRE(
        plan.resolution_plan->render_source_resource ==
        "low_color");
    REQUIRE(
        plan.resolution_plan->output_source_resource ==
        "display");
    REQUIRE(
        plan.resolution_plan->render_extent ==
        ResourceExtentPlan{
            .kind =
                ResourceExtentKind::output_relative,
            .scale_x = 0.5f,
            .scale_y = 0.5f,
        });
    REQUIRE(
        physicalResource(plan, "low_color").extent ==
        plan.resolution_plan->render_extent);
    REQUIRE(
        vulkanTargetPlanToJson(plan)
            .at("resolution_plan")
            .at("render_source_resource") ==
        "low_color");
}

TEST_CASE(
    "scene resolution domain rejects incompatible physical extents",
    "[target-planning][upscale][resolution]") {
    const auto config = nlohmann::json{
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "invalid_scene_extents"},
               {"passes",
                nlohmann::json::array(
                    {{{"name", "produce_scene"},
                      {"type", "raster"},
                      {"resolution_domain", "scene"},
                      {"output",
                       {{"color",
                         nlohmann::json::array(
                             {"half_color",
                              "three_quarter_color"})},
                        {"depth", nullptr}}}},
                     {{"name", "present"},
                      {"type", "fullscreen"},
                      {"input",
                       nlohmann::json::array(
                           {"half_color"})},
                      {"output",
                       {{"color", "display"},
                        {"depth", nullptr}}}}})}}})},
    };
    auto half =
        target("half_color",
               ImageFormat::r8g8b8a8_unorm);
    half.extent_scale = 0.5f;
    auto three_quarter =
        target("three_quarter_color",
               ImageFormat::r8g8b8a8_unorm);
    three_quarter.extent_scale = 0.75f;
    const std::vector targets{
        half, three_quarter,
        target("display",
               ImageFormat::b8g8r8a8_srgb),
    };

    requireThrowsContaining(
        [&] {
            const auto graphs =
                TestSupport::renderGraphOf(
                    config);
            (void)compileRenderingTargetPlans(
                withImagesOf(graphs, targets), targets,
                SampleCountPolicy{},
                vk::Format::eB8G8R8A8Unorm,
                RenderingTargetPlanDeviceFacts{
                    .query_attachment_samples =
                        [](const auto &) {
                            return std::vector<
                                std::uint32_t>{1};
                        },
                });
        },
        "scene resolution domain has incompatible physical extents");
}

TEST_CASE("runtime target adapter carries multiview device facts and typed view requests",
          "[wp203][target-planning][multiview][runtime-adapter]") {
    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto compilation = compileRenderingTargetPlans(
        withImagesOf(graphs, targets), targets, SampleCountPolicy{},
        vk::Format::eB8G8R8A8Unorm,
        RenderingTargetPlanDeviceFacts{
            .multiview = true,
            .max_multiview_view_count = 2,
            .device_identity = {
                .vendor_id = 4318,
                .device_id = 9860,
                .driver_version = 77,
                .device_name = "Mock GPU",
            },
            .query_attachment_samples =
                [](const auto &) {
                    return std::vector<std::uint32_t>{1};
                },
        },
        VulkanViewExecutionPlanRequest{
            .view_count = 2,
            .preference =
                XrViewExecutionPreference::automatic,
            .automatic_policy = {
                .profiles =
                    {XrMultiviewDeviceProfile{
                        .id =
                            "runtime_adapter_fast",
                        .vendor_id = 4318,
                        .device_id = 9860,
                        .measurement = {
                            .sequential_gpu_ms = 6.0,
                            .multiview_gpu_ms = 4.0,
                            .sample_count = 240,
                            .source =
                                "RenderTiming/gpu_timestamp",
                        },
                    }},
            },
            .multiview_capable_nodes =
                {"geometry", "lighting", "forward", "present"},
        },
        VulkanExternalDepthExportRequest{});

    REQUIRE(compilation.plans.size() == 1);
    const auto &plan = *compilation.plans.front();
    REQUIRE(plan.view_execution_plan.uses_multiview);
    REQUIRE(plan.view_execution_plan.view_count == 2);
    REQUIRE(plan.view_execution_plan
                .automatic_policy
                .matched_profile);
    REQUIRE(plan.view_execution_plan
                .automatic_policy
                .profile_id ==
            "runtime_adapter_fast");
    REQUIRE(physicalResource(plan, "lit").view_layout ==
            VulkanResourceViewLayout::layered_2d_array);
    REQUIRE(physicalResource(plan, "lit").array_layers == 2);
    REQUIRE(layerAssignment(compilation, "lit").array_layers == 2);
    REQUIRE(layerAssignment(compilation, "display").array_layers == 2);
    REQUIRE(plan.external_depth_export.has_value());
    REQUIRE(plan.external_depth_export->source_resource ==
            "depth");
    REQUIRE(plan.external_depth_export->format ==
            ImageFormat::d32_sfloat);
    REQUIRE(plan.external_depth_export->array_layers == 2);

    auto materialized_targets = targets;
    applyPlan(materialized_targets, compilation);
    const auto lit = std::find_if(
        materialized_targets.begin(), materialized_targets.end(),
        [](const auto &candidate) {
            return candidate.name == "lit";
        });
    REQUIRE(lit != materialized_targets.end());
    REQUIRE(lit->array_layers == 2);
    const auto depth = std::find_if(
        materialized_targets.begin(),
        materialized_targets.end(),
        [](const auto &candidate) {
            return candidate.name == "depth";
        });
    REQUIRE(depth != materialized_targets.end());
    REQUIRE(depth->usage &
            ImageUsageBit::transfer_src);
}

TEST_CASE("runtime depth export stays disabled when the device format cannot be a transfer source",
          "[target-planning][external-depth][fallback]") {
    const auto config = hybridConfig();
    const auto graphs =
        TestSupport::renderGraphOf(config);
    const std::vector targets{
        target("albedo", ImageFormat::b8g8r8a8_unorm),
        target("normal", ImageFormat::r16g16b16a16_sfloat),
        target("custom_id", ImageFormat::r32_uint),
        target("depth", ImageFormat::d32_sfloat,
               ImageUsageBit::depth_stencil_attachment),
        target("lit", ImageFormat::r16g16b16a16_sfloat),
        target("display", ImageFormat::b8g8r8a8_srgb),
    };
    const auto compilation =
        compileRenderingTargetPlans(
            withImagesOf(graphs, targets), targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<std::uint32_t>{1};
                    },
                .supports_external_depth_transfer =
                    [](const auto &) {
                        return false;
                    },
            },
            std::nullopt,
            VulkanExternalDepthExportRequest{});

    REQUIRE(compilation.plans.size() == 1);
    REQUIRE_FALSE(
        compilation.plans.front()
            ->external_depth_export);
    auto materialized_targets = targets;
    applyPlan(
        materialized_targets, compilation);
    const auto depth = std::find_if(
        materialized_targets.begin(),
        materialized_targets.end(),
        [](const auto &candidate) {
            return candidate.name == "depth";
        });
    REQUIRE(depth != materialized_targets.end());
    REQUIRE_FALSE(
        depth->usage &
        ImageUsageBit::transfer_src);
}

// PS-1: a persistent target keeps its contents across frames, so it is never
// an alias candidate, and the plan's reason says why. A target that is a
// candidate without it is one again when it is transient.
TEST_CASE(
    "PS-1 a persistent target is kept out of runtime aliasing with persistent lifetime as the reason",
    "[target-planning][alias][runtime][ps1]") {
    auto display =
        target("display",
               ImageFormat::r8g8b8a8_unorm);
    display.format_class = "display";
    const auto compile = [&](bool persistent) {
        auto temporary_b =
            target("temporary_b",
                   ImageFormat::r8g8b8a8_unorm);
        temporary_b.persistent = persistent;
        const std::vector targets{
            target("temporary_a",
                   ImageFormat::r8g8b8a8_unorm),
            temporary_b,
            display,
        };
        return compileRenderingTargetPlans(
            withImagesOf(TestSupport::renderGraphOf(
                             aliasLifetimeConfig()),
                         targets),
            targets, SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            RenderingTargetPlanDeviceFacts{
                .query_attachment_samples =
                    [](const auto &) {
                        return std::vector<
                            std::uint32_t>{1};
                    },
            });
    };
    const auto kept = compile(true);
    const auto &plan = *kept.plans.front();
    CHECK(plan.alias_groups.empty());
    CHECK(physicalResource(plan, "temporary_b").reason ==
          "logical materialization requirement is required: "
          "persistent lifetime");
    CHECK_FALSE(physicalResource(plan, "temporary_b").aliasable);
    // Control: transient, the same two targets share one allocation.
    const auto shared = compile(false);
    REQUIRE(shared.plans.front()->alias_groups.size() == 1);
    CHECK(physicalResource(*shared.plans.front(), "temporary_b").reason ==
          "arbitrary read requires a materialized resource");

    // The JSON form: lifetime is persistent or transient, nothing else.
    const auto parsed = Pelican::test_support::renderTargetDefinitionsOf(nlohmann::json{
        {"render_targets",
         nlohmann::json::array(
             {{{"name", "kept"}, {"format", "R8G8B8A8_UNORM"}, {"extent_scale", 1.0},
               {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}, {"lifetime", "persistent"}},
              {{"name", "frame"}, {"format", "R8G8B8A8_UNORM"}, {"extent_scale", 1.0},
               {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}, {"lifetime", "transient"}},
              {{"name", "plain"}, {"format", "R8G8B8A8_UNORM"}, {"extent_scale", 1.0},
               {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}}})}});
    REQUIRE(parsed.size() == 3);
    CHECK(parsed[0].persistent);
    CHECK_FALSE(parsed[1].persistent);
    CHECK_FALSE(parsed[2].persistent);
    CHECK_THROWS_WITH(
        Pelican::test_support::renderTargetDefinitionsOf(nlohmann::json{
            {"render_targets",
             nlohmann::json::array(
                 {{{"name", "kept"}, {"format", "R8G8B8A8_UNORM"}, {"extent_scale", 1.0},
                   {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}, {"lifetime", "forever"}}})}}),
        Catch::Matchers::ContainsSubstring(R"(render_graph.image_invalid@1 {"field":"lifetime","reason":"not_persistent_or_transient","resource":"kept"})"));
}

// PS-1: a frame that skips a once node records nothing for it, so target
// planning gives it a physical scope of its own, whatever the authored
// target_planning says.
TEST_CASE(
    "PS-1 a once pass is isolated in its own physical scope",
    "[target-planning][runtime][ps1]") {
    const auto compile = [](bool once) {
        auto config = localReadAttachmentConfig();
        if (once) {
            config["rendering_passes"][0]["passes"][1]["schedule"] = "once";
        }
        const auto render_graph =
            TestSupport::renderGraphOf(config);
        const auto graphs = TestSupport::frameGraphsOf(render_graph);
        REQUIRE(graphs.size() == 1);
        for (const auto &node : graphs.front().nodes)
            CHECK(node.once == (once && node.name == "lighting"));
        return compileRenderingTargetPlans(
            render_graph, Pelican::test_support::renderTargetDefinitionsOf(config),
            SampleCountPolicy{},
            vk::Format::eB8G8R8A8Unorm,
            localReadDeviceFacts());
    };
    const auto scopeOf = [](const VulkanTargetPlan &plan, std::string_view node) {
        const auto found = std::find_if(
            plan.scopes.begin(), plan.scopes.end(),
            [&](const auto &scope) {
                return std::find(scope.nodes.begin(), scope.nodes.end(), node) != scope.nodes.end();
            });
        REQUIRE(found != plan.scopes.end());
        return found->nodes;
    };
    // Control: every frame, the geometry and its same-pixel reader share a
    // tile-local scope.
    const auto fused = compile(false);
    CHECK(scopeOf(*fused.plans.front(), "lighting") ==
          std::vector<std::string>{"geometry", "lighting"});
    // Once, the reader is alone: a frame that skips it begins nothing.
    const auto separate = compile(true);
    CHECK(scopeOf(*separate.plans.front(), "lighting") ==
          std::vector<std::string>{"lighting"});
    CHECK(scopeOf(*separate.plans.front(), "geometry") ==
          std::vector<std::string>{"geometry"});
}

} // namespace Pelican
