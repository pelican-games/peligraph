#include "../src/core/renderingpass/renderingsamplecount.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

void requireThrowsContaining(const std::function<void()> &operation,
                             std::string_view expected) {
    try {
        operation();
        FAIL("operation did not throw");
    } catch (const std::runtime_error &error) {
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

} // namespace Pelican
