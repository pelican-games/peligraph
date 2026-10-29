// RC-3a1: the engine's image vocabulary (ImageFormat, ImageUsage) and the
// one place core maps it to Vulkan. Uses Vulkan's format traits to check the
// table, so it is a core test (a bridge until RC-3c2 moves the mapping's test
// next to the rest of core's).

#include "../src/core/renderingpass/materialpassattachments.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "../src/core/renderingpass/rendertargetdefinition.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/vulkanimagevocabulary.hpp"
#include "../src/project/imageformat.hpp"
#include "../src/project/imageusage.hpp"
#include "../src/project/targetrenderplanning.hpp"
#include "../src/project/vulkancompletephysicalplan.hpp"
#include "registered_render_targets_test_support.hpp"
#include "render_graph_ir_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <vulkan/vulkan_format_traits.hpp>

namespace Pelican {
namespace {

std::string upper(std::string_view text) {
    std::string result{text};
    for (auto &c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}

ImageAspect vulkanAspect(vk::Format format) {
    bool depth = false;
    bool stencil = false;
    for (std::uint8_t component = 0; component < vk::componentCount(format); ++component) {
        const std::string_view name = vk::componentName(format, component);
        depth = depth || name == "D";
        stencil = stencil || name == "S";
    }
    if (depth) return stencil ? ImageAspect::depth_stencil : ImageAspect::depth;
    return stencil ? ImageAspect::stencil : ImageAspect::color;
}

} // namespace

TEST_CASE("RC-3a1 every row of the image format table is what Vulkan's format traits say",
          "[rc3a1][image-vocabulary]") {
    const auto table = imageFormatTable();
    REQUIRE(table.size() == 46);
    std::set<vk::Format> mapped;
    for (const auto &row : table) {
        INFO(std::string{row.name});
        const auto vulkan = toVulkanFormat(row.format);
        CHECK(mapped.insert(vulkan).second);
        CHECK(imageFormatFromVulkan(vulkan) == row.format);
        CHECK(findImageFormat(row.name) == row.format);
        CHECK(row.aspect == vulkanAspect(vulkan));
        REQUIRE(row.component_count == vk::componentCount(vulkan));
        for (std::uint8_t component = 0; component < row.component_count; ++component) {
            INFO("component " << int{component});
            CHECK(upper(imageComponentStorageName(imageFormatComponentStorage(row.format, component))) ==
                  vk::componentNumericFormat(vulkan, component));
        }
        CHECK(row.estimated_texel_bytes > 0);
    }
    // The bytes are RC-1's estimate, not Vulkan's texel block size.
    CHECK(imageFormatEstimatedTexelBytes(ImageFormat::d32_sfloat_s8_uint) == 8);
    CHECK(vk::blockSize(toVulkanFormat(ImageFormat::d32_sfloat_s8_uint)) == 5);
    // A shader sees float of every storage but the integer ones.
    CHECK(imageFormatShaderNumeric(ImageFormat::b10g11r11_ufloat_pack32) == ImageShaderNumeric::floating);
    CHECK(imageFormatShaderNumeric(ImageFormat::r8g8b8a8_srgb) == ImageShaderNumeric::floating);
    CHECK(imageFormatShaderNumeric(ImageFormat::r16_sint) == ImageShaderNumeric::signed_integer);
    CHECK(imageFormatShaderNumeric(ImageFormat::r32g32_uint) == ImageShaderNumeric::unsigned_integer);
    CHECK(imageFormatShaderNumeric(ImageFormat::d24_unorm_s8_uint) == ImageShaderNumeric::floating);
    CHECK(imageFormatComponentStorage(ImageFormat::d32_sfloat_s8_uint, 0) == ImageComponentStorage::sfloat);
    CHECK(imageFormatComponentStorage(ImageFormat::d32_sfloat_s8_uint, 1) == ImageComponentStorage::uint);

    for (const auto &row : imageUsageTable()) {
        INFO(std::string{row.name});
        CHECK(imageUsageFromVulkan(toVulkanImageUsage(row.bit)) == ImageUsage{row.bit});
    }
}

TEST_CASE("RC-3a1 a render target declaration accepts and refuses the formats and usages it did",
          "[rc3a1][image-vocabulary]") {
    const auto formats = authoredImageFormatNames();
    REQUIRE(formats.size() == 45);
    CHECK(formats.front() == "R8_UNORM");
    CHECK(formats.back() == "D32_SFLOAT");
    CHECK(std::find(formats.begin(), formats.end(), "D32_SFLOAT_S8_UINT") == formats.end());
    for (const auto &name : formats) CHECK(std::string{imageFormatName(parseAuthoredImageFormat(name))} == name);
    CHECK_THROWS_WITH(parseAuthoredImageFormat("D32_SFLOAT_S8_UINT"),
                      "Unknown format: D32_SFLOAT_S8_UINT");
    CHECK_THROWS_WITH(parseAuthoredImageFormat("R16G16B16A16Sfloat"),
                      "Unknown format: R16G16B16A16Sfloat");

    CHECK(authoredImageUsageNames() ==
          std::vector<std::string>{"COLOR_ATTACHMENT", "DEPTH_STENCIL_ATTACHMENT", "SAMPLED",
                                   "STORAGE", "TRANSFER_DST", "TRANSFER_SRC"});
    const std::vector<std::string> input{"INPUT_ATTACHMENT"};
    CHECK_THROWS_WITH(parseAuthoredImageUsage(input), "Unknown image usage flag: INPUT_ATTACHMENT");
    CHECK_THROWS_WITH(parseAuthoredImageUsage({}), "Image usage flags must not be empty");

    // The render target parser itself.
    const auto declared = [](std::string format) {
        return nlohmann::json{{"render_targets",
                               nlohmann::json::array({{{"name", "t"},
                                                       {"format", std::move(format)},
                                                       {"format_class", "data"},
                                                       {"extent_scale", 1.0},
                                                       {"usage", {"COLOR_ATTACHMENT"}}}})}};
    };
    CHECK(Pelican::test_support::renderTargetDefinitionsOf(declared("B10G11R11_UFLOAT_PACK32")).front().format ==
          ImageFormat::b10g11r11_ufloat_pack32);
    CHECK_THROWS_WITH(Pelican::test_support::renderTargetDefinitionsOf(declared("D32_SFLOAT_S8_UINT")),
                      R"(render_graph.image_invalid@1 {"field":"format","reason":"unknown","resource":"t"})");

    // v0 wrote ejected physical plans in Vulkan's spelling; one reader moves
    // them to the author's.
    CHECK(parsePhysicalPlanImageFormat("R16G16B16A16Sfloat", "test") == ImageFormat::r16g16b16a16_sfloat);
    CHECK(parsePhysicalPlanImageFormat("B10G11R11UfloatPack32", "test") ==
          ImageFormat::b10g11r11_ufloat_pack32);
    CHECK(parsePhysicalPlanImageFormat("D24UnormS8Uint", "test") == ImageFormat::d24_unorm_s8_uint);
    CHECK(parsePhysicalPlanImageFormat("D32_SFLOAT_S8_UINT", "test") == ImageFormat::d32_sfloat_s8_uint);
    CHECK_THROWS_WITH(parsePhysicalPlanImageFormat("R16G16B16A16", "fragment format"),
                      "fragment format names an unknown image format: R16G16B16A16");
}

namespace {

// Four targets the production loader reads, one graph writing each.
nlohmann::json vocabularyConfig() {
    return {
        {"render_targets",
         nlohmann::json::array({
             {{"name", "ids"}, {"format", "R8_UINT"}, {"format_class", "data"},
              {"extent_scale", 1.0}, {"width", 64}, {"height", 32},
              {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
             {{"name", "lit"}, {"format", "R16G16B16A16_SFLOAT"}, {"format_class", "data"},
              {"format_candidates", {"R8G8B8A8_UNORM"}}, {"extent_scale", 0.5},
              {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
             {{"name", "depth"}, {"format", "D32_SFLOAT"}, {"format_class", "data"},
              {"extent_scale", 0.5}, {"usage", {"DEPTH_STENCIL_ATTACHMENT"}}},
             {{"name", "trail"}, {"format", "R16G16_SFLOAT"}, {"format_class", "data"},
              {"extent_scale", 1.0}, {"history", true},
              {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
             {{"name", "display"}, {"format", "B8G8R8A8_SRGB"}, {"format_class", "data"},
              {"extent_scale", 1.0}, {"usage", {"COLOR_ATTACHMENT", "SAMPLED"}}},
         })},
        {"rendering_passes",
         nlohmann::json::array(
             {{{"name", "main"},
               {"passes",
                nlohmann::json::array({
                    {{"name", "id_pass"}, {"type", "fullscreen"},
                     {"output", {{"color", nlohmann::json::array({"ids"})}, {"depth", nullptr}}}},
                    {{"name", "forward"}, {"type", "material"},
                     {"output", {{"color", nlohmann::json::array({"lit"})}, {"depth", "depth"}}}},
                    {{"name", "trail_pass"}, {"type", "fullscreen"},
                     {"output", {{"color", nlohmann::json::array({"trail"})}, {"depth", nullptr}}}},
                    {{"name", "present"}, {"type", "fullscreen"}, {"input", {"lit", "ids", "trail"}},
                     {"output", {{"color", nlohmann::json::array({"display"})}, {"depth", nullptr}}}},
                })}}})},
    };
}

struct ConsumedImage {
    vk::Format format;
    vk::ImageUsageFlags usage;
    vk::Extent2D extent;
    MaterialOutputNumericClass numeric;
    vk::ResolveModeFlagBits resolve;
};

// What registration hands Vulkan for one applied definition at a
// 1920x1080 output, and what the passes read of its numeric type.
ConsumedImage consumed(const RenderTargetDefinition &definition) {
    const auto extent = resolveResourceExtent(
        definition.fixed_extent
            ? ResourceExtentPlan{.kind = ResourceExtentKind::fixed,
                                 .width = definition.fixed_extent->width,
                                 .height = definition.fixed_extent->height}
            : ResourceExtentPlan{.kind = ResourceExtentKind::output_relative,
                                 .scale_x = definition.extent_scale,
                                 .scale_y = definition.extent_scale},
        ResolvedResourceExtent{1920, 1080});
    return ConsumedImage{
        .format = toVulkanFormat(definition.format),
        .usage = toVulkanImageUsage(definition.usage),
        .extent = toVulkanExtent(extent),
        .numeric = materialPassFormatNumericClass(definition.format),
        .resolve = colorAttachmentResolveMode(definition.format),
    };
}

const RenderTargetDefinition &named(std::span<const RenderTargetDefinition> targets,
                                    std::string_view name) {
    const auto found = std::find_if(targets.begin(), targets.end(),
                                    [&](const auto &target) { return target.name == name; });
    REQUIRE(found != targets.end());
    return *found;
}

const VulkanPhysicalResourcePlan &planned(const VulkanTargetPlan &plan, std::string_view name) {
    const auto found = std::find_if(plan.resources.begin(), plan.resources.end(),
                                    [&](const auto &resource) { return resource.logical_resource == name; });
    REQUIRE(found != plan.resources.end());
    return *found;
}

} // namespace

TEST_CASE("RC-3a1 the loader, the planner and registration consume the declared image vocabulary",
          "[rc3a1][image-vocabulary][target-planning]") {
    const auto config = vocabularyConfig();
    const auto targets = Pelican::test_support::renderTargetDefinitionsOf(config);
    const auto graphs = TestSupport::renderGraphOf(config);
    const auto facts = RenderingTargetPlanDeviceFacts{
        .query_image_format_capability =
            [](const RenderTargetDefinition &) {
                return RenderingImageFormatCapability{
                    .image_usage_supported = true,
                    .supported_samples = {1},
                    .max_array_layers = 4,
                };
            },
    };
    const auto automatic = compileRenderingTargetPlans(
        graphs, Pelican::TestSupport::framePlanMapOf(graphs), targets, compileSampleCountPolicy(config), vk::Format::eB8G8R8A8Srgb, facts);
    REQUIRE(automatic.plans.size() == 1);
    const auto &plan = *automatic.plans.front();

    // The plan's formats are the declared ones; its dump spells them as the
    // author does (D3).
    CHECK(planned(plan, "ids").format == ImageFormat::r8_uint);
    CHECK(planned(plan, "lit").format == ImageFormat::r16g16b16a16_sfloat);
    const auto dump = vulkanTargetPlanToJson(plan);
    const auto dumped = [&](std::string_view name) {
        for (const auto &resource : dump.at("resources"))
            if (resource.at("logical_resource").get<std::string>() == name) return resource.at("format").get<std::string>();
        FAIL("no dumped resource " << name);
        return std::string{};
    };
    CHECK(dumped("ids") == "R8_UINT");
    CHECK(dumped("depth") == "D32_SFLOAT");
    CHECK(dumped("trail") == "R16G16_SFLOAT");

    SECTION("without a format selection") {
        const auto applied = test_support::plannedRenderTargets(graphs, automatic);
        const auto ids = consumed(named(applied, "ids"));
        CHECK(ids.format == vk::Format::eR8Uint);
        CHECK(ids.usage == (vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled));
        CHECK(ids.extent == vk::Extent2D{64, 32});
        CHECK(ids.numeric == MaterialOutputNumericClass::unsigned_integer);
        CHECK(ids.resolve == vk::ResolveModeFlagBits::eSampleZero);

        const auto lit = consumed(named(applied, "lit"));
        CHECK(lit.format == vk::Format::eR16G16B16A16Sfloat);
        CHECK(lit.extent == vk::Extent2D{960, 540});
        CHECK(lit.numeric == MaterialOutputNumericClass::floating);
        CHECK(lit.resolve == vk::ResolveModeFlagBits::eAverage);

        const auto depth = consumed(named(applied, "depth"));
        CHECK(depth.format == vk::Format::eD32Sfloat);
        CHECK(depth.usage == vk::ImageUsageFlags{vk::ImageUsageFlagBits::eDepthStencilAttachment});
        CHECK(depth.extent == vk::Extent2D{960, 540});

        // Registration adds the transfer destination a history image is
        // cleared through.
        const auto trail = consumed(named(applied, "trail"));
        CHECK(trail.format == vk::Format::eR16G16Sfloat);
        CHECK(trail.usage == (vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
                              vk::ImageUsageFlagBits::eTransferDst));
        CHECK(trail.extent == vk::Extent2D{1920, 1080});
    }


    SECTION("with a complete physical plan selecting the declared candidate") {
        REQUIRE(automatic.verification_contexts.size() == 1);
        const auto &context = automatic.verification_contexts.front();
        auto document = vulkanCompletePhysicalPlanPackageToJson(ejectVulkanCompletePhysicalPlanPackage(plan));
        bool edited = false;
        for (auto &resource : document.at("resources"))
            if (resource.at("logical_resource") == "lit") {
                CHECK(resource.at("format") == "R16G16B16A16_SFLOAT");
                resource["format"] = "R8G8B8A8_UNORM";
                edited = true;
            }
        REQUIRE(edited);
        const auto verified = verifyVulkanCompletePhysicalPlanPackage(
            *context.logical_graph, context.topology, *context.automatic_plan, context.node_order,
            vulkanCompletePhysicalPlanPackageFromJson(document), context.format_capabilities);
        const auto installed = applyVerifiedVulkanCompletePhysicalPlanPackage(*context.automatic_plan, verified);
        CHECK(planned(installed, "lit").format == ImageFormat::r8g8b8a8_unorm);
        CHECK(planned(installed, "ids").format == ImageFormat::r8_uint);
    }
}

} // namespace Pelican
