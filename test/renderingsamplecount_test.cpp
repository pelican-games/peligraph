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
    const RenderingSampleCountResolution &resolution,
    std::string_view resource) {
    const auto found = std::find_if(
        resolution.assignments.begin(), resolution.assignments.end(),
        [resource](const auto &candidate) {
            return candidate.resource == resource;
        });
    REQUIRE(found != resolution.assignments.end());
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
    const auto resolution = resolveRenderingSampleCounts(
        config, targets, compileSampleCountPolicy(config), query);
    REQUIRE(assignment(resolution, "albedo").samples == 2);
    REQUIRE(assignment(resolution, "normal").samples == 2);
    REQUIRE(assignment(resolution, "custom_id").samples == 2);
    REQUIRE(assignment(resolution, "depth").samples == 2);
    REQUIRE(assignment(resolution, "lit").samples == 2);
    REQUIRE(assignment(resolution, "display").samples == 1);
    REQUIRE(resolution.plan.groups.at(0).limiting_resources ==
            std::vector<std::string>{"custom_id (R32Uint)"});
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
            (void)resolveRenderingSampleCounts(
                config, targets, compileSampleCountPolicy(config),
                [](const RenderTargetDefinition &definition) {
                    return definition.name == "custom_id"
                               ? std::vector<std::uint32_t>{1}
                               : std::vector<std::uint32_t>{1, 2, 4};
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
    const auto resolution = resolveRenderingSampleCounts(
        config, targets, compileSampleCountPolicy(config),
        [](const auto &) {
            return std::vector<std::uint32_t>{1, 2, 4};
        });
    REQUIRE(assignment(resolution, "albedo").samples == 4);
    REQUIRE(assignment(resolution, "lit").samples == 4);
    REQUIRE(assignment(resolution, "display").samples == 1);
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
