// RC-3a2: an attachment's load / store ops, from the author's pass through the
// planner, the physical plan and registration to what the recording hands
// beginRendering.
#include "../src/core/renderingpass/frameplanner.hpp"
#include "../src/core/renderingpass/renderingpassconfigjsonparser.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetdefinition.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/vkcore/recordedattachmentops.hpp"
#include "../src/project/attachmentoperations.hpp"
#include "../src/project/renderingjsonnormalize.hpp"
#include "../src/project/targetrenderplanning.hpp"
#include "../src/project/vulkancompletephysicalplan.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {

namespace {

using Json = nlohmann::json;
using VkLoad = vk::AttachmentLoadOp;
using VkStore = vk::AttachmentStoreOp;

Json target(std::string name, std::string format, std::vector<std::string> usage) {
    return Json{{"name", std::move(name)}, {"format", std::move(format)}, {"format_class", "data"},
                {"extent_scale", 1.0}, {"width", 64}, {"height", 64}, {"usage", std::move(usage)}};
}

Json attachment(std::string name) { return Json{{"target", std::move(name)}}; }

// A material pass's own outputs, one per color attachment, so any count of
// them is a valid material pass.
Json materialOutputs(std::size_t count) {
    auto outputs = Json::array();
    for (std::size_t index = 0; index < count; ++index)
        outputs.push_back(Json{{"name", "out" + std::to_string(index)}, {"type", "vec4"}, {"source", "custom"}});
    return Json{{"schema", "pelican.material_outputs"}, {"version", 1}, {"name", "rc3a2.outputs"},
                {"outputs", std::move(outputs)}};
}

// Four material passes, each on its own attachments (so none is fused into
// another's rendering scope), in the normal form the parsers receive:
//   defaults        a + d       no ops anywhere
//   pass_wide       b + d2      the pass's own ops only
//   per_attachment  c, a + d3   the attachments' own over the pass's
//   scratch_pass    scratch     a write-only target the plan makes transient
//                   + d4
Json operationsConfig() {
    auto c = attachment("c");
    c["load_op"] = "clear";
    auto a = attachment("a");
    a["load_op"] = "load";
    a["store_op"] = "store";
    auto d3 = attachment("d3");
    d3["load_op"] = "clear";
    d3["store_op"] = "store";
    auto scratch = attachment("scratch");
    scratch["store_op"] = "store";
    const std::vector<std::string> sampled_color{"COLOR_ATTACHMENT", "SAMPLED"};
    const std::vector<std::string> sampled_depth{"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"};
    return Json{
        {"render_targets",
         Json::array({target("a", "R8G8B8A8_UNORM", sampled_color), target("b", "R8G8B8A8_UNORM", sampled_color),
                      target("c", "R8G8B8A8_UNORM", sampled_color), target("d", "D32_SFLOAT", sampled_depth),
                      target("d2", "D32_SFLOAT", sampled_depth), target("d3", "D32_SFLOAT", sampled_depth), target("d4", "D32_SFLOAT", sampled_depth),
                      target("scratch", "R8G8B8A8_UNORM", {"COLOR_ATTACHMENT"})})},
        {"rendering_passes",
         Json::array({Json{
             {"name", "main"},
             {"passes",
              Json::array({
                  Json{{"name", "defaults"},
                       {"type", "material"},
                       {"material_contract", "forward_opaque_v1"},
                       {"material_outputs", materialOutputs(1)},
                       {"output", {{"color", Json::array({attachment("a")})}, {"depth", attachment("d")}}}},
                  Json{{"name", "pass_wide"},
                       {"type", "material"},
                       {"material_contract", "forward_opaque_v1"},
                       {"material_outputs", materialOutputs(1)},
                       {"color_load_op", "dont_care"},
                       {"color_store_op", "store"},
                       {"depth_load_op", "clear"},
                       {"depth_store_op", "store"},
                       {"output", {{"color", Json::array({attachment("b")})}, {"depth", attachment("d2")}}}},
                  Json{{"name", "per_attachment"},
                       {"type", "material"},
                       {"material_contract", "forward_opaque_v1"},
                       {"material_outputs", materialOutputs(2)},
                       {"color_load_op", "dont_care"},
                       {"color_store_op", "dont_care"},
                       {"depth_load_op", "load"},
                       {"output", {{"color", Json::array({c, a})}, {"depth", d3}}}},
                  Json{{"name", "scratch_pass"},
                       {"type", "material"},
                       {"material_contract", "forward_opaque_v1"},
                       {"material_outputs", materialOutputs(1)},
                       {"output", {{"color", Json::array({scratch})}, {"depth", attachment("d4")}}}},
              })}}})},
    };
}

// The recorded ops of one pass: its color attachments in order, then its
// depth attachment if it has one.
struct Recorded {
    std::vector<RecordedAttachmentOps> color;
    std::optional<RecordedAttachmentOps> depth;

    bool operator==(const Recorded &) const = default;
};

RecordedAttachmentOps ops(VkLoad load, VkStore store) { return RecordedAttachmentOps{load, store}; }

// What each pass's attachments must record, written from the rule
// (attachment > pass > the material pass's defaults: color clear / store,
// depth clear / dont_care), not from the code.
std::map<std::string, Recorded> byPriority() {
    return {
        {"defaults", {{ops(VkLoad::eClear, VkStore::eStore)}, ops(VkLoad::eClear, VkStore::eDontCare)}},
        {"pass_wide", {{ops(VkLoad::eDontCare, VkStore::eStore)}, ops(VkLoad::eClear, VkStore::eStore)}},
        {"per_attachment",
         {{ops(VkLoad::eClear, VkStore::eDontCare), ops(VkLoad::eLoad, VkStore::eStore)},
          ops(VkLoad::eClear, VkStore::eStore)}},
        {"scratch_pass", {{ops(VkLoad::eClear, VkStore::eStore)}, ops(VkLoad::eClear, VkStore::eDontCare)}},
    };
}

struct Registered {
    std::vector<RenderTargetDefinition> targets;
    std::vector<RenderTargetMetadata> metadata;
    std::map<std::string, GlobalRenderTargetId, std::less<>> ids;
};

Registered registered(const std::vector<RenderTargetDefinition> &declared,
                      const RenderingTargetPlanCompilation &compilation) {
    Registered result;
    result.targets = declared;
    applyRenderingTargetPlan(result.targets, compilation);
    for (std::size_t index = 0; index < result.targets.size(); ++index) {
        const auto &definition = result.targets[index];
        result.ids.emplace(definition.name, GlobalRenderTargetId{static_cast<int>(index)});
        result.metadata.push_back(RenderTargetMetadata{
            .name = definition.name,
            .usage = registeredRenderTargetUsage(definition),
            .format = definition.format,
            .extent = *definition.fixed_extent,
            .samples = definition.samples,
            .storage_mode = definition.storage_mode,
        });
    }
    return result;
}

// Registration's pass parse and runtime compile against `plan` (nullptr: no
// physical contract), and what the recording then hands beginRendering.
std::map<std::string, Recorded> recorded(const Json &config, const Registered &targets,
                                         const VulkanTargetPlan *plan) {
    const RenderTargetNameResolver names{[&](const std::string &name) {
        const auto found = targets.ids.find(name);
        return found == targets.ids.end() ? noRenderTargetId() : found->second;
    }};
    const RenderTargetMetadataResolver metadata{[&](GlobalRenderTargetId id) {
        return targets.metadata.at(static_cast<std::size_t>(id.value));
    }};
    const auto definitions = parseRenderingPassDefinitionsFromConfigJson(config, names, metadata);
    REQUIRE(definitions.size() == 1);
    const auto compiled = compileRenderingPassRuntime(
        definitions.front(),
        RenderingPassRuntimeDependencies{.render_target_metadata = &metadata, .target_plan = plan});
    std::map<std::string, Recorded> result;
    for (const auto &pass : compiled.passes) {
        Recorded ops;
        for (std::size_t index = 0; index < pass.definition.output_color.size(); ++index)
            ops.color.push_back(recordedColorAttachmentOps(pass.definition, index));
        if (isConcreteRenderTarget(pass.definition.output_depth))
            ops.depth = recordedDepthAttachmentOps(pass.definition);
        // The scope's own (a single pass's scope is that pass's ops).
        if (plan != nullptr) {
            REQUIRE(pass.rendering.scope_color_attachment_operations.size() == ops.color.size());
            for (std::size_t index = 0; index < ops.color.size(); ++index)
                CHECK(recordedAttachmentOps(pass.rendering.scope_color_attachment_operations[index]) ==
                      ops.color[index]);
        }
        result.emplace(pass.definition.name, std::move(ops));
    }
    return result;
}

const VulkanPhysicalResourcePlan &planned(const VulkanTargetPlan &plan, std::string_view name) {
    const auto found = std::find_if(plan.resources.begin(), plan.resources.end(),
                                    [&](const auto &resource) { return resource.logical_resource == name; });
    REQUIRE(found != plan.resources.end());
    return *found;
}

RenderingTargetPlanDeviceFacts deviceFacts() {
    return RenderingTargetPlanDeviceFacts{
        .query_image_format_capability =
            [](const RenderTargetDefinition &definition) {
                return RenderingImageFormatCapability{
                    .image_usage_supported = true,
                    .supported_samples = {1},
                    .max_array_layers = 4,
                    .transient_attachment_supported = definition.name == "scratch",
                };
            },
        .transient_attachments = true,
    };
}

} // namespace

TEST_CASE("RC-3a2 the loader, the planner and registration hand the recording the attachment ops they resolve",
          "[rc3a2][attachment-operations][target-planning]") {
    const auto config = operationsConfig();
    // The parsers receive the normal form (RC-3b).
    REQUIRE_NOTHROW(requireNormalizedRenderingJson(config));
    const auto declared = parseRenderTargetDefinitionsFromJson(config);
    const auto graphs = parseFrameGraphDefinitionsFromConfigJson(config);
    const auto facts = deviceFacts();
    const auto automatic =
        compileRenderingTargetPlans(graphs, declared, compileSampleCountPolicy(config), vk::Format::eB8G8R8A8Srgb, facts);
    REQUIRE(automatic.plans.size() == 1);
    const auto &plan = *automatic.plans.front();
    CHECK(planned(plan, "a").representation == VulkanResourceRepresentation::materialized_image);
    CHECK(planned(plan, "d3").representation == VulkanResourceRepresentation::materialized_image);
    REQUIRE(planned(plan, "scratch").representation == VulkanResourceRepresentation::transient_attachment);

    SECTION("automatic plan: attachment > pass > defaults; a transient target's store is elided") {
        auto expected = byPriority();
        // The plan's own change: nothing reads the transient scratch target.
        expected.at("scratch_pass").color.at(0) = ops(VkLoad::eClear, VkStore::eDontCare);
        CHECK(recorded(config, registered(declared, automatic), &plan) == expected);
        // The plan's dump spells dont_care as the physical plan does.
        const auto dump = vulkanTargetPlanToJson(plan);
        bool found = false;
        for (const auto &entry : dump.at("attachments"))
            if (entry.at("node") == "scratch_pass") {
                CHECK(entry.at("store_op") == "discard");
                found = true;
            }
        CHECK(found);
    }

    SECTION("no physical contract: the pass's own resolution (the execution side's fallback)") {
        CHECK(recorded(config, registered(declared, automatic), nullptr) == byPriority());
    }

    SECTION("a physical fragment's edit reaches the recording") {
        auto document = vulkanPhysicalFragmentPackageToJson(ejectVulkanPhysicalFragmentPackage(plan));
        bool edited = false;
        for (auto &entry : document.at("attachments")) {
            if (entry.at("node") == "defaults" && entry.at("logical_resource") == "a") {
                CHECK(entry.at("load_op") == "clear");
                entry["load_op"] = "discard";
                edited = true;
            }
        }
        REQUIRE(edited);
        const std::array fragments{vulkanPhysicalFragmentPackageFromJson(document)};
        const auto linked = compileRenderingTargetPlans(graphs, declared, compileSampleCountPolicy(config),
                                                        vk::Format::eB8G8R8A8Srgb, facts, std::nullopt,
                                                        std::nullopt, {}, {}, fragments);
        REQUIRE(linked.plans.size() == 1);
        auto expected = byPriority();
        expected.at("scratch_pass").color.at(0) = ops(VkLoad::eClear, VkStore::eDontCare);
        expected.at("defaults").color.at(0) = ops(VkLoad::eDontCare, VkStore::eStore);
        CHECK(recorded(config, registered(declared, linked), linked.plans.front().get()) == expected);

        // The wire's spellings are its own: "dont_care" is not one.
        for (auto &entry : document.at("attachments"))
            if (entry.at("node") == "defaults" && entry.at("logical_resource") == "a") entry["load_op"] = "dont_care";
        CHECK_THROWS_WITH(vulkanPhysicalFragmentPackageFromJson(document),
                          "Vulkan physical attachment fragment has unknown load_op: dont_care");
    }

    SECTION("a complete physical plan reads and writes the plan's spellings") {
        auto document = vulkanCompletePhysicalPlanPackageToJson(ejectVulkanCompletePhysicalPlanPackage(plan));
        std::map<std::string, std::string> stores;
        for (const auto &entry : document.at("attachments"))
            stores[entry.at("node").get<std::string>() + "/" + entry.at("logical_resource").get<std::string>()] =
                entry.at("store_op").get<std::string>();
        CHECK(stores.at("defaults/d") == "discard");
        CHECK(stores.at("scratch_pass/scratch") == "discard");
        CHECK(stores.at("pass_wide/d2") == "store");
        const auto read = vulkanCompletePhysicalPlanPackageFromJson(document);
        CHECK(read.attachments.size() == plan.attachments.size());
        for (const auto &attachment : read.attachments) {
            if (attachment.node != "defaults") continue;
            CHECK(attachment.operations ==
                  (attachment.aspect == AttachmentAspect::depth
                       ? PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::dont_care}
                       : PassAttachmentOperations{AttachmentLoadOp::clear, AttachmentStoreOp::store}));
        }
        for (auto &entry : document.at("attachments"))
            if (entry.at("node") == "defaults" && entry.at("logical_resource") == "d") entry["store_op"] = "dont_care";
        CHECK_THROWS_WITH(vulkanCompletePhysicalPlanPackageFromJson(document),
                          Catch::Matchers::ContainsSubstring(
                              "complete physical attachment has unknown store_op 'dont_care'"));
    }
}

TEST_CASE("RC-3a2 the pass parsers admit the authored op spellings and refuse the rest by name",
          "[rc3a2][attachment-operations][spelling]") {
    // The planner's and registration's parsers read a pass's ops through
    // one conversion: every authored spelling, and nothing else.
    const auto planned_op = [](const Json &config) {
        const auto graphs = parseFrameGraphDefinitionsFromConfigJson(config);
        return graphs.front().nodes.front().attachments.front().operations;
    };
    for (const auto &[spelling, op] : std::vector<std::pair<std::string, AttachmentLoadOp>>{
             {"Clear", AttachmentLoadOp::clear},
             {"Load", AttachmentLoadOp::load},
             {"DontCare", AttachmentLoadOp::dont_care},
             {"dontCare", AttachmentLoadOp::dont_care}}) {
        CAPTURE(spelling);
        auto config = operationsConfig();
        config["rendering_passes"][0]["passes"][0]["color_load_op"] = spelling;
        CHECK(planned_op(config).load_op == op);
    }
    auto config = operationsConfig();
    config["rendering_passes"][0]["passes"][0]["color_load_op"] = "discard";
    CHECK_THROWS_WITH(parseFrameGraphDefinitionsFromConfigJson(config), "Unknown attachment load op: discard");
    config = operationsConfig();
    config["rendering_passes"][0]["passes"][0]["output"]["color"][0]["store_op"] = "discard";
    CHECK_THROWS_WITH(parseFrameGraphDefinitionsFromConfigJson(config), "Unknown attachment store op: discard");
}

} // namespace Pelican
