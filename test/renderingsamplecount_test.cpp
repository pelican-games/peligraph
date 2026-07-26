#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"

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

RenderTargetDefinition target(
    std::string name, vk::Format format,
    vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eColorAttachment |
        vk::ImageUsageFlagBits::eSampled) {
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
                     {{"name", "lighting"},
                      {"type", "fullscreen"},
                      {"output",
                       {{"color", nlohmann::json::array({"lit"})},
                        {"depth", nullptr}}}},
                     {{"name", "forward"},
                      {"type", "material"},
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
    return {
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
                      {"type", std::move(consumer_type)},
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
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment |
                   vk::ImageUsageFlagBits::eTransferSrc),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    const auto query = [](const RenderTargetDefinition &definition) {
        if (definition.name == "custom_id") {
            return std::vector<std::uint32_t>{1, 2};
        }
        return std::vector<std::uint32_t>{1, 2, 4};
    };

    const auto config = hybridConfig();
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    const auto compilation = compileRenderingTargetPlans(
        graphs, targets, compileSampleCountPolicy(config),
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
            std::vector<std::string>{"custom_id (R32Uint)"});
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
            .load_op ==
        VulkanPhysicalAttachmentLoadOp::clear);
    REQUIRE(
        physicalAttachment(
            *compilation.plans.front(),
            "geometry", "depth")
            .store_op ==
        VulkanPhysicalAttachmentStoreOp::discard);
    REQUIRE(
        ejectVulkanPhysicalFragmentPackage(
            *compilation.plans.front())
            .schema_version == 2);
}

TEST_CASE("rendering target bridge links attachment operations into the physical plan",
          "[target-planning][rendering][physical-attachment][bridge]") {
    const std::vector targets{
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment |
                   vk::ImageUsageFlagBits::eTransferSrc),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    const auto config = hybridConfig();
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
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
            graphs, targets,
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
        VulkanPhysicalAttachmentLoadOp::discard;

    const std::array fragments{fragment};
    const auto linked =
        compileRenderingTargetPlans(
            graphs, targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            facts,
            std::nullopt, std::nullopt, {},
            {}, fragments);
    REQUIRE(
        physicalAttachment(
            *linked.plans.front(),
            "geometry", "albedo")
            .load_op ==
        VulkanPhysicalAttachmentLoadOp::discard);
    REQUIRE(
        linked.plans.front()
            ->applied_fragment_package ==
        std::optional<
            VulkanPhysicalFragmentPackage>{
            fragment});
}

TEST_CASE(
    "runtime target adapter executes supported write-only attachments as transient",
    "[target-planning][rendering][transient-attachment][runtime-adapter]") {
    const auto config =
        writeOnlyAttachmentConfig();
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            config);
    auto scratch =
        target(
            "scratch",
            vk::Format::eR8G8B8A8Unorm,
            vk::ImageUsageFlagBits::
                eColorAttachment);
    scratch.format_candidates = {
        vk::Format::eR16G16B16A16Sfloat};
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
            graphs, targets,
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
            .store_op ==
        VulkanPhysicalAttachmentStoreOp::discard);
    REQUIRE(
        representationAssignment(
            transient, "scratch")
            .representation ==
        VulkanResourceRepresentation::
            transient_attachment);

    auto applied_targets = targets;
    applyRenderingTargetPlan(
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
                    vk::Format::
                        eR16G16B16A16Sfloat,
            };
        };
    const auto unsupported =
        compileRenderingTargetPlans(
            graphs, targets,
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
            graphs, targets,
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
            .store_op ==
        VulkanPhysicalAttachmentStoreOp::store);

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
        vk::to_string(
            vk::Format::
                eR16G16B16A16Sfloat);
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
                    vk::Format::
                        eR8G8B8A8Unorm,
            };
        };
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                graphs, targets,
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
        parseFrameGraphDefinitionsFromConfigJson(
            config);
    const auto targets =
        parseRenderTargetDefinitionsFromJson(
            config);

    const auto local =
        compileRenderingTargetPlans(
            graphs, targets, SampleCountPolicy{},
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
            .store_op ==
        VulkanPhysicalAttachmentStoreOp::
            discard);

    auto applied = targets;
    applyRenderingTargetPlan(applied, local);
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
        vk::ImageUsageFlagBits::
            eInputAttachment);

    const auto missing_feature =
        compileRenderingTargetPlans(
            graphs, targets, SampleCountPolicy{},
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
            graphs, targets, SampleCountPolicy{},
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
    "runtime local-read eligibility rejects unsupported consumers extents and multisampling",
    "[target-planning][rendering][tile-local][fallback]") {
    const auto compile_config =
        [](const nlohmann::json &config,
           SampleCountPolicy samples = {}) {
            const auto graphs =
                parseFrameGraphDefinitionsFromConfigJson(
                    config);
            const auto targets =
                parseRenderTargetDefinitionsFromJson(
                    config);
            return compileRenderingTargetPlans(
                graphs, targets, samples,
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
        "pelican.vulkan.materialized_plan@1");
    REQUIRE(
        representationAssignment(
            material_consumer, "gbuffer")
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
        parseFrameGraphDefinitionsFromConfigJson(
            config);
    const auto targets =
        parseRenderTargetDefinitionsFromJson(
            config);
    auto device_facts = localReadDeviceFacts();
    device_facts.multiview = true;
    device_facts.max_multiview_view_count = 2;

    const auto compilation =
        compileRenderingTargetPlans(
            graphs, targets, SampleCountPolicy{},
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
    applyRenderingTargetPlan(applied, compilation);
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
        vk::ImageUsageFlagBits::eInputAttachment);
}

TEST_CASE(
    "runtime target bridge materializes a shared target without losing local-read usage",
    "[target-planning][rendering][tile-local][multi-graph]") {
    auto config = localReadAttachmentConfig();
    auto material_graph =
        config["rendering_passes"].front();
    material_graph["name"] =
        "material_fallback";
    material_graph["passes"][1]["type"] =
        "material";
    config["rendering_passes"].push_back(
        std::move(material_graph));

    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            config);
    const auto targets =
        parseRenderTargetDefinitionsFromJson(
            config);
    const auto compilation =
        compileRenderingTargetPlans(
            graphs, targets, SampleCountPolicy{},
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
    applyRenderingTargetPlan(
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
        vk::ImageUsageFlagBits::
            eInputAttachment);
    REQUIRE(
        gbuffer->usage &
        vk::ImageUsageFlagBits::eSampled);
}

TEST_CASE("rendering target bridge applies a verified alternate physical format",
          "[target-planning][rendering][physical-format][bridge]") {
    auto targets = std::vector{
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment |
                   vk::ImageUsageFlagBits::eTransferSrc),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    const auto lit = std::find_if(
        targets.begin(), targets.end(),
        [](const auto &candidate) {
            return candidate.name == "lit";
        });
    REQUIRE(lit != targets.end());
    lit->format_candidates = {
        vk::Format::eR16G16B16A16Sfloat,
        vk::Format::eR8G8B8A8Unorm,
    };

    const auto config = hybridConfig();
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
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
            graphs, targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            facts);
    REQUIRE(
        formatAssignment(automatic, "lit").format ==
        vk::Format::eR16G16B16A16Sfloat);

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
        vk::to_string(
            vk::Format::eR8G8B8A8Unorm);
    const std::array fragments{fragment};
    const auto linked =
        compileRenderingTargetPlans(
            graphs, targets,
            compileSampleCountPolicy(config),
            vk::Format::eB8G8R8A8Unorm,
            facts,
            std::nullopt, std::nullopt, {},
            {}, fragments);
    REQUIRE(
        formatAssignment(linked, "lit").format ==
        vk::Format::eR8G8B8A8Unorm);
    REQUIRE(
        physicalResource(
            *linked.plans.front(), "lit")
            .format ==
        vk::to_string(
            vk::Format::eR8G8B8A8Unorm));
    REQUIRE(
        std::any_of(
            linked.plans.front()
                ->sample_count_plan->resources.begin(),
            linked.plans.front()
                ->sample_count_plan->resources.end(),
            [](const auto &candidate) {
                return candidate.resource == "lit" &&
                       candidate.format ==
                           vk::to_string(
                               vk::Format::
                                   eR8G8B8A8Unorm);
            }));

    auto applied_targets = targets;
    applyRenderingTargetPlan(
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
            vk::Format::eR8G8B8A8Unorm);

    auto unsupported_facts = facts;
    unsupported_facts
        .query_image_format_capability =
        [](const RenderTargetDefinition &definition) {
            return RenderingImageFormatCapability{
                .image_usage_supported =
                    definition.format !=
                    vk::Format::eR8G8B8A8Unorm,
                .supported_samples = {1, 2, 4},
                .max_array_layers = 4,
            };
        };
    requireThrowsContaining(
        [&] {
            (void)compileRenderingTargetPlans(
                graphs, targets,
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
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment |
                   vk::ImageUsageFlagBits::eTransferSrc),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    const auto config = hybridConfig();
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
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
        graphs, targets, compileSampleCountPolicy(config),
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
            graphs, targets,
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
            graphs, targets,
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
                graphs, targets,
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
                graphs, targets,
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
                graphs, targets,
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
                graphs, targets, compileSampleCountPolicy(config),
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
                graphs, targets, compileSampleCountPolicy(config),
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
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    requireThrowsContaining(
        [&] {
            const auto graphs =
                parseFrameGraphDefinitionsFromConfigJson(config);
            (void)compileRenderingTargetPlans(
                graphs, targets,
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
        "custom_id (R32Uint)");
}

TEST_CASE("explicit target selects its whole attachment component",
          "[sample-count][rendering]") {
    auto config = hybridConfig();
    config["multisampling"]["scope"] = "none";
    config["multisampling"]["targets"] = nlohmann::json::array({"lit"});
    const std::vector targets{
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    const auto compilation = compileRenderingTargetPlans(
        graphs, targets, compileSampleCountPolicy(config),
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
    REQUIRE(colorAttachmentResolveMode(vk::Format::eR32Uint) ==
            vk::ResolveModeFlagBits::eSampleZero);
    REQUIRE(colorAttachmentResolveMode(vk::Format::eR16Sint) ==
            vk::ResolveModeFlagBits::eSampleZero);
    REQUIRE(colorAttachmentResolveMode(
                vk::Format::eR16G16B16A16Sfloat) ==
            vk::ResolveModeFlagBits::eAverage);
    REQUIRE(colorAttachmentResolveMode(
                vk::Format::eB8G8R8A8Unorm) ==
            vk::ResolveModeFlagBits::eAverage);
}

TEST_CASE(
    "runtime target bridge applies compatible lifetime alias groups",
    "[target-planning][alias][runtime]") {
    auto display =
        target("display",
               vk::Format::eR8G8B8A8Unorm);
    display.format_class = "display";
    std::vector targets{
        target("temporary_a",
               vk::Format::eR8G8B8A8Unorm),
        target("temporary_b",
               vk::Format::eR8G8B8A8Unorm),
        display,
    };
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            aliasLifetimeConfig());
    const auto compilation =
        compileRenderingTargetPlans(
            graphs, targets, SampleCountPolicy{},
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
        applyRenderingTargetPlan(
            duplicate_targets, duplicate_group),
        Catch::Matchers::ContainsSubstring(
            "duplicate physical alias group assignment id"));

    applyRenderingTargetPlan(targets, compilation);
    REQUIRE(targets[0].alias_group.has_value());
    REQUIRE(targets[1].alias_group ==
            targets[0].alias_group);
    REQUIRE_FALSE(targets[2].alias_group.has_value());
}

TEST_CASE(
    "runtime alias merge drops groups that graph variants do not share",
    "[target-planning][alias][runtime][variant]") {
    auto display =
        target("display",
               vk::Format::eR8G8B8A8Unorm);
    display.format_class = "display";
    const std::vector targets{
        target("temporary_a",
               vk::Format::eR8G8B8A8Unorm),
        target("temporary_b",
               vk::Format::eR8G8B8A8Unorm),
        display,
    };
    auto alias_graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            aliasLifetimeConfig());
    auto single_graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            aliasLifetimeConfig(false));
    alias_graphs.insert(
        alias_graphs.end(),
        single_graphs.begin(),
        single_graphs.end());

    const auto compilation =
        compileRenderingTargetPlans(
            alias_graphs, targets,
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
               vk::Format::eR8G8B8A8Unorm);
    low.extent_scale = 0.5f;
    auto display =
        target("display",
               vk::Format::eB8G8R8A8Srgb);
    const std::vector targets{low, display};
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    const auto compilation =
        compileRenderingTargetPlans(
            graphs, targets, SampleCountPolicy{},
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
                      {"type", "fullscreen"},
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
               vk::Format::eR8G8B8A8Unorm);
    half.extent_scale = 0.5f;
    auto three_quarter =
        target("three_quarter_color",
               vk::Format::eR8G8B8A8Unorm);
    three_quarter.extent_scale = 0.75f;
    const std::vector targets{
        half, three_quarter,
        target("display",
               vk::Format::eB8G8R8A8Srgb),
    };

    requireThrowsContaining(
        [&] {
            const auto graphs =
                parseFrameGraphDefinitionsFromConfigJson(
                    config);
            (void)compileRenderingTargetPlans(
                graphs, targets,
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
        parseFrameGraphDefinitionsFromConfigJson(config);
    const std::vector targets{
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    const auto compilation = compileRenderingTargetPlans(
        graphs, targets, SampleCountPolicy{},
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
            vk::to_string(vk::Format::eD32Sfloat));
    REQUIRE(plan.external_depth_export->array_layers == 2);

    auto materialized_targets = targets;
    applyRenderingTargetPlan(materialized_targets, compilation);
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
            vk::ImageUsageFlagBits::eTransferSrc);
}

TEST_CASE("runtime depth export stays disabled when the device format cannot be a transfer source",
          "[target-planning][external-depth][fallback]") {
    const auto config = hybridConfig();
    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(config);
    const std::vector targets{
        target("albedo", vk::Format::eB8G8R8A8Unorm),
        target("normal", vk::Format::eR16G16B16A16Sfloat),
        target("custom_id", vk::Format::eR32Uint),
        target("depth", vk::Format::eD32Sfloat,
               vk::ImageUsageFlagBits::eDepthStencilAttachment),
        target("lit", vk::Format::eR16G16B16A16Sfloat),
        target("display", vk::Format::eB8G8R8A8Srgb),
    };
    const auto compilation =
        compileRenderingTargetPlans(
            graphs, targets, SampleCountPolicy{},
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
    applyRenderingTargetPlan(
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
        vk::ImageUsageFlagBits::eTransferSrc);
}

} // namespace Pelican
