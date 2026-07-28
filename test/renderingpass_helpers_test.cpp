#include "../src/core/renderingpass/fullscreenpassinfojsonparser.hpp"
#include "../src/core/renderingpass/computetask.hpp"
#include "../src/core/renderingpass/materialpassattachments.hpp"
#include "../src/core/renderingpass/materialpassinfojsonparser.hpp"
#include "../src/core/renderingpass/passattachmentoptionsjsonparser.hpp"
#include "../src/core/renderingpass/passdefinitionjsonparser.hpp"
#include "../src/core/renderingpass/passinfojsonparser.hpp"
#include "../src/core/renderingpass/passsequencejsonparser.hpp"
#include "../src/core/renderingpass/renderingpassjsonhelpers.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/renderingpassruntimecompiler.hpp"
#include "../src/core/renderingpass/renderingpasstargetjsonparser.hpp"
#include "../src/core/renderingpass/renderingpassvalidation.hpp"
#include "../src/core/renderingpass/rendertargetconfigregistration.hpp"
#include "../src/core/renderingpass/rendertargetmetadataresolver.hpp"
#include "../src/core/renderingpass/rendertargetnameresolver.hpp"
#include "../src/core/renderingpass/rendertargetjsonparser.hpp"
#include "../src/core/renderingpass/viewexecutionscheduler.hpp"
#include "../src/core/userpublic/render/pass_implementation_abi_v1.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_set>
#include <variant>
#include <vector>

namespace Pelican {

TEST_CASE("rendering pass helper ids name invalid and special targets", "[renderingpass]") {
    REQUIRE(invalidRenderingPassId().value == invalidRenderingPassIdValue);
    REQUIRE_FALSE(isValidRenderingPassId(invalidRenderingPassId()));
    REQUIRE(isValidRenderingPassId(RenderingPassId{0}));

    REQUIRE(noRenderTargetId().value == invalidRenderTargetIdValue);
    REQUIRE(swapchainRenderTargetId().value == swapchainRenderTargetIdValue);
    REQUIRE_FALSE(isConcreteRenderTarget(noRenderTargetId()));
    REQUIRE_FALSE(isConcreteRenderTarget(swapchainRenderTargetId()));
    REQUIRE(isSpecialRenderTarget(noRenderTargetId()));
    REQUIRE(isSpecialRenderTarget(swapchainRenderTargetId()));
    REQUIRE(isSwapchainRenderTarget(swapchainRenderTargetId()));
}

TEST_CASE("rendering pass JSON helpers parse known values", "[renderingpass]") {
    REQUIRE(stringToFormat("R8_UNORM") == vk::Format::eR8Unorm);
    REQUIRE(stringToFormat("D32_SFLOAT") == vk::Format::eD32Sfloat);
    REQUIRE(
        stringToFormat("R32_SFLOAT") ==
        vk::Format::eR32Sfloat);
    REQUIRE(
        formatToString(
            vk::Format::eR32Sfloat) ==
        "R32_SFLOAT");

    const auto usage = stringToUsageFlags({"COLOR_ATTACHMENT", "SAMPLED"});
    REQUIRE(static_cast<bool>(usage & vk::ImageUsageFlagBits::eColorAttachment));
    REQUIRE(static_cast<bool>(usage & vk::ImageUsageFlagBits::eSampled));

    REQUIRE(std::holds_alternative<MaterialPassInfo>(makePassInfo("material")));
    REQUIRE(std::holds_alternative<FullscreenPassInfo>(makePassInfo("fullscreen")));
    REQUIRE(std::holds_alternative<ShadowDepthPassInfo>(makePassInfo("shadow_depth")));
    REQUIRE(std::holds_alternative<UiPassInfo>(makePassInfo("ui")));

    REQUIRE(stringToFullscreenPushConstantData("projection_view") == FullscreenPushConstantData::eProjectionView);
    REQUIRE(stringToLoadOp("dont_care") == vk::AttachmentLoadOp::eDontCare);
    REQUIRE(stringToStoreOp("store") == vk::AttachmentStoreOp::eStore);

    const nlohmann::json object{
        {"name", "main_pass"},
        {"scale", 1.5},
        {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
        {"count", 42},
    };
    REQUIRE(parseStringField(object, "name", "test") == "main_pass");
    REQUIRE(parseFloatField(object, "scale", "test") == 1.5f);
    REQUIRE(parseStringArrayField(object, "usage", "test").size() == 2);
    REQUIRE(parseUint32Field(object, "count", "test") == 42);
    REQUIRE(
        parseOptionalRegionTags(
            nlohmann::json{
                {"regions",
                 nlohmann::json::array(
                     {"region.post", "region.debug"})}},
            "test") ==
        std::vector<std::string>{
            "region.post", "region.debug"});
    REQUIRE_THROWS_WITH(
        parseOptionalRegionTags(
            nlohmann::json{
                {"regions",
                 nlohmann::json::array(
                     {"region.post", "region.post"})}},
            "test"),
        Catch::Matchers::ContainsSubstring(
            "duplicate region tag"));
    REQUIRE_THROWS_WITH(
        parseOptionalRegionTags(
            nlohmann::json{
                {"regions",
                 nlohmann::json::array({""})}},
            "test"),
        Catch::Matchers::ContainsSubstring(
            "empty or too long"));

    const auto clear_color = jsonToClearColor(nlohmann::json::array({1.0f, 0.5f, 0.25f, 1.0f}));
    REQUIRE(clear_color[0] == 1.0);
    REQUIRE(clear_color[1] == 0.5);
    REQUIRE(clear_color[2] == 0.25);
    REQUIRE(clear_color[3] == 1.0);
}

TEST_CASE("render target JSON parser returns target definitions", "[renderingpass]") {
    const nlohmann::json config{
        {"render_targets",
         nlohmann::json::array({{
             {"name", "half_color"},
             {"extent_scale", 0.5},
             {"format", "R8_UNORM"},
             {"format_candidates",
              nlohmann::json::array(
                  {"R8_UNORM",
                   "R16G16_SFLOAT"})},
             {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
         }})},
    };

    const auto definitions = parseRenderTargetDefinitionsFromJson(config);

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].name == "half_color");
    REQUIRE(definitions[0].extent_scale == 0.5f);
    REQUIRE_FALSE(definitions[0].fixed_extent.has_value());
    REQUIRE(definitions[0].format == vk::Format::eR8Unorm);
    REQUIRE(
        definitions[0].format_candidates ==
        std::vector<vk::Format>{
            vk::Format::eR8Unorm,
            vk::Format::eR16G16Sfloat});
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eColorAttachment));
    REQUIRE(static_cast<bool>(definitions[0].usage & vk::ImageUsageFlagBits::eSampled));
}

TEST_CASE("render target JSON parser rejects malformed format candidates",
          "[renderingpass][physical-format]") {
    auto config = nlohmann::json{
        {"render_targets",
         nlohmann::json::array({{
             {"name", "color"},
             {"extent_scale", 1.0},
             {"format", "R8G8B8A8_UNORM"},
             {"usage",
              nlohmann::json::array(
                  {"COLOR_ATTACHMENT"})},
             {"format_candidates", "R16G16B16A16_SFLOAT"},
         }})},
    };
    REQUIRE_THROWS_WITH(
        parseRenderTargetDefinitionsFromJson(
            config),
        Catch::Matchers::ContainsSubstring(
            "format_candidates must be a string array"));
}

TEST_CASE("render target JSON parser accepts fixed extents", "[renderingpass]") {
    const nlohmann::json config{
        {"render_targets",
         nlohmann::json::array({{
             {"name", "shadow_map"},
             {"extent_scale", 1.0},
             {"width", 2048},
             {"height", 2048},
             {"format", "D32_SFLOAT"},
             {"usage", nlohmann::json::array({"DEPTH_STENCIL_ATTACHMENT", "SAMPLED"})},
         }})},
    };

    const auto definitions = parseRenderTargetDefinitionsFromJson(config);

    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions[0].fixed_extent.has_value());
    REQUIRE(definitions[0].fixed_extent->width == 2048);
    REQUIRE(definitions[0].fixed_extent->height == 2048);
}

TEST_CASE("render target JSON parser accepts history and its declarative clear",
          "[renderingpass][temporal]") {
    const auto definitions = parseRenderTargetDefinitionsFromJson(nlohmann::json{
        {"render_targets", nlohmann::json::array({{
            {"name", "temporal_accum"}, {"extent_scale", 1.0},
            {"format", "R16G16B16A16_SFLOAT"},
            {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})},
            {"history", true}, {"clear_color", {0.1f, 0.2f, 0.3f, 1.0f}}
        }})}
    });
    REQUIRE(definitions.size() == 1);
    REQUIRE(definitions.front().history);
    REQUIRE(definitions.front().history_clear_color[0] == Catch::Approx(0.1));
    REQUIRE(definitions.front().history_clear_color[3] == Catch::Approx(1.0));
}

TEST_CASE(
    "history clear is encoded after integer target format selection",
    "[renderingpass][temporal][typed-clear][wp218]") {
    const auto definitions =
        parseRenderTargetDefinitionsFromJson(
            nlohmann::json{
                {"render_targets",
                 nlohmann::json::array(
                     {{{"name", "history_id"},
                       {"extent_scale", 1.0},
                       {"format", "R32_UINT"},
                       {"usage",
                        nlohmann::json::array(
                            {"COLOR_ATTACHMENT",
                             "SAMPLED"})},
                       {"history", true},
                       {"clear_color",
                        {4294967295.0, 3, 0, 0}}}})}});
    REQUIRE(definitions.size() == 1);
    const auto physical =
        physicalRenderTargetHistoryClearColor(
            definitions.front());
    REQUIRE(
        physical.uint32[0] ==
        std::numeric_limits<
            std::uint32_t>::max());
    REQUIRE(physical.uint32[1] == 3u);

    auto invalid = definitions.front();
    invalid.history_clear_color[0] = 0.5;
    REQUIRE_THROWS_WITH(
        physicalRenderTargetHistoryClearColor(
            invalid),
        Catch::Matchers::ContainsSubstring(
            "fractional or out of range"));
}

TEST_CASE("resolver v2 selects B8 SRGB scene/display and float HDR", "[renderingpass][color-c1b]") {
    const auto config = nlohmann::json{
        {"resolver_version", 2},
        {"render_targets",
         nlohmann::json::array({
             {{"name", "lit_color"},
              {"extent_scale", 1.0},
              {"format", "B8G8R8A8_UNORM"},
              {"format_class", "scene"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED"})}},
             {{"name", "display"},
              {"extent_scale", 1.0},
              {"format", "B8G8R8A8_SRGB"},
              {"format_class", "display"},
              {"usage", nlohmann::json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"})}},
         })},
    };

    const auto resolved = resolveRenderTargetFormatClassesV2(
        config, vk::Format::eR8G8B8A8Srgb, vk::Extent2D{64, 32}, false);
    REQUIRE(resolved.at("render_targets").at(0).at("format").get<std::string>() ==
            "B8G8R8A8_SRGB");
    REQUIRE(resolved.at("render_targets").at(1).at("format").get<std::string>() ==
            "B8G8R8A8_SRGB");
    REQUIRE(resolved.at("render_targets").at(1).at("width").get<uint32_t>() == 64);
    REQUIRE(resolved.at("render_targets").at(1).at("height").get<uint32_t>() == 32);
    REQUIRE(parseRenderTargetDefinitionsFromJson(resolved).at(0).format ==
            vk::Format::eB8G8R8A8Srgb);
    const auto hdr = resolveRenderTargetFormatClassesV2(
        config, vk::Format::eR8G8B8A8Srgb, vk::Extent2D{64, 32}, true);
    REQUIRE(hdr.at("render_targets").at(0).at("format") == "R16G16B16A16_SFLOAT");
}

TEST_CASE("fullscreen pass JSON parser reads explicit fullscreen options", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"push_constants", "camera_position"},
        {"uses_light_data", true},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}},
    };

    const auto info = parseFullscreenPassInfoFromJson(pass_json, "lighting_pass");

    REQUIRE(info.push_constants == FullscreenPushConstantData::eCameraPosition);
    REQUIRE(info.uses_light_data);
    REQUIRE(info.vert_shader.ref == "fullscreen");
    REQUIRE(info.vert_shader.kind == ShaderReferenceKind::stem);
    REQUIRE(info.frag_shader.ref == "lighting");
    REQUIRE(info.frag_shader.kind == ShaderReferenceKind::stem);
}

TEST_CASE(
    "render target JSON parser keeps mip and array-layer contracts",
    "[renderingpass][subresource][wp209b]") {
    const auto definitions =
        parseRenderTargetDefinitionsFromJson(
            nlohmann::json::parse(R"json({
              "render_targets": [
                {
                  "name": "fixed_pyramid",
                  "extent_scale": 1.0,
                  "format": "R16G16_SFLOAT",
                  "usage": ["SAMPLED", "STORAGE"],
                  "mip_levels": 6,
                  "layers": 3
                },
                {
                  "name": "full_pyramid",
                  "extent_scale": 1.0,
                  "format": "R16G16_SFLOAT",
                  "usage": ["SAMPLED", "STORAGE"],
                  "mip_levels": "full"
                }
              ]
            })json"));
    REQUIRE(definitions.size() == 2);
    const ImageMipLevelCount fixed{
        ImageMipLevelMode::fixed, 6};
    const ImageMipLevelCount full{
        ImageMipLevelMode::full_chain, 1};
    REQUIRE(
        definitions[0].mip_levels ==
        fixed);
    REQUIRE(
        definitions[1].mip_levels ==
        full);
    REQUIRE(definitions[0].array_layers == 3);
    REQUIRE(definitions[1].array_layers == 1);
    REQUIRE(resolveImageMipLevels(
                definitions[1].mip_levels,
                128, 64) == 8);

    auto invalid = nlohmann::json::parse(R"json({
      "render_targets": [{
        "name": "bad",
        "extent_scale": 1.0,
        "format": "R16G16_SFLOAT",
        "usage": ["SAMPLED"],
        "mip_levels": 0
      }]
    })json");
    REQUIRE_THROWS_WITH(
        parseRenderTargetDefinitionsFromJson(
            invalid),
        Catch::Matchers::ContainsSubstring(
            "mip_levels must be positive"));
    invalid["render_targets"][0]["mip_levels"] =
        "automatic";
    REQUIRE_THROWS_WITH(
        parseRenderTargetDefinitionsFromJson(
            invalid),
        Catch::Matchers::ContainsSubstring(
            "must be 'full'"));
    invalid["render_targets"][0]["mip_levels"] = 1;
    invalid["render_targets"][0]["layers"] = 0;
    REQUIRE_THROWS_WITH(
        parseRenderTargetDefinitionsFromJson(
            invalid),
        Catch::Matchers::ContainsSubstring(
            "layers must be positive"));
}

TEST_CASE(
    "fullscreen pass JSON parser keeps per-input sampling typed",
    "[renderingpass][upscale][sampling]") {
    const nlohmann::json pass_json{
        {"input_sampling",
         nlohmann::json::array(
             {{{"filter", "nearest"},
               {"address", "clamp_to_edge"}},
              {{"filter", "linear"},
               {"address", "mirrored_repeat"}}})},
        {"shader",
         {{"vertex", "fullscreen"},
          {"fragment", "upscale"}}},
    };

    const auto info =
        parseFullscreenPassInfoFromJson(
            pass_json, "upscale");

    REQUIRE(info.input_sampling ==
            std::vector<FullscreenInputSampling>{
                {FullscreenInputFilter::nearest,
                 FullscreenInputAddressMode::clamp_to_edge},
                {FullscreenInputFilter::linear,
                 FullscreenInputAddressMode::mirrored_repeat},
            });
    auto invalid = pass_json;
    invalid["input_sampling"][0]["address"] =
        "wrap_somehow";
    REQUIRE_THROWS_AS(
        parseFullscreenPassInfoFromJson(
            invalid, "upscale"),
        std::runtime_error);
}

TEST_CASE(
    "fullscreen pass JSON parser keeps named resource ports independent "
    "from graph edges",
    "[renderingpass][resource-port][wp207a]") {
    const auto pass_json =
        nlohmann::json::parse(R"json({
          "input": ["scene_color", "scene_depth@history"],
          "resource_ports": {
            "color": {
              "resource": "scene_color",
              "access": "sampled",
              "sampling": {
                "filter": "nearest",
                "address": "clamp_to_edge"
              }
            },
            "previous_depth": {
              "resource": "scene_depth@history",
              "view": "per_view"
            }
          },
          "shader": {
            "vertex": "fullscreen",
            "fragment": "resolve"
          }
        })json");

    const auto info =
        parseFullscreenPassInfoFromJson(
            pass_json, "resolve");
    REQUIRE(info.resource_ports.size() == 2);
    REQUIRE(
        info.resource_ports.at(0).name == "color");
    REQUIRE(
        info.resource_ports.at(0).resource ==
        "scene_color");
    REQUIRE(
        info.resource_ports.at(0).sampling.filter ==
        ShaderResourcePortFilter::nearest);
    REQUIRE(
        info.resource_ports.at(0)
            .sampling.address_mode ==
        ShaderResourcePortAddressMode::
            clamp_to_edge);
    REQUIRE(
        info.resource_ports.at(1).view ==
        ShaderResourcePortView::per_view);

    auto conflicting = pass_json;
    conflicting["input_sampling"] =
        nlohmann::json::array(
            {{{"filter", "linear"}}});
    REQUIRE_THROWS_WITH(
        parseFullscreenPassInfoFromJson(
            conflicting, "resolve"),
        Catch::Matchers::ContainsSubstring(
            "resource_ports sampling replaces input_sampling"));

    auto unknown = pass_json;
    unknown["resource_ports"]["color"]["resource"] =
        "not_an_input";
    REQUIRE_THROWS_WITH(
        parseFullscreenPassInfoFromJson(
            unknown, "resolve"),
        Catch::Matchers::ContainsSubstring(
            "absent from reads/writes/input"));

    auto duplicate = pass_json;
    duplicate["resource_ports"]["color"]["subresource"] =
        {{"mip", 0}};
    duplicate["resource_ports"]["previous_depth"]
             ["resource"] = "scene_color";
    duplicate["resource_ports"]["previous_depth"]
             ["access"] = "sampled";
    duplicate["resource_ports"]["previous_depth"]
             ["subresource"] = {{"mip", 1}};
    REQUIRE_THROWS_WITH(
        parseFullscreenPassInfoFromJson(
            duplicate, "resolve"),
        Catch::Matchers::ContainsSubstring(
            "cannot map multiple ports"));
}

TEST_CASE(
    "fullscreen pass JSON parser accepts explicit read-only buffer ports",
    "[renderingpass][resource-port][clustered][wp208]") {
    const auto pass_json =
        nlohmann::json::parse(R"json({
          "input": ["scene_color", "light_inventory"],
          "resource_ports": {
            "color": {
              "resource": "scene_color",
              "access": "sampled"
            },
            "lights": {
              "resource": "light_inventory",
              "kind": "buffer",
              "element": "uvec4",
              "access": "storage"
            }
          },
          "shader": {
            "vertex": "fullscreen",
            "fragment": "lighting"
          }
        })json");

    const auto info =
        parseFullscreenPassInfoFromJson(
            pass_json, "lighting");
    REQUIRE(info.resource_ports.size() == 2);
    REQUIRE(
        info.resource_ports.at(1).kind ==
        ShaderResourcePortKind::buffer);
    REQUIRE(
        info.resource_ports.at(1).buffer_element ==
        ShaderResourceBufferElement::uvec4);

    auto invalid = pass_json;
    invalid["resource_ports"]["lights"]["access"] =
        "sampled";
    REQUIRE_THROWS_WITH(
        parseFullscreenPassInfoFromJson(
            invalid, "lighting"),
        Catch::Matchers::ContainsSubstring(
            "buffer ports require storage access"));
}

TEST_CASE(
    "shader resource ports type disjoint mip reads and writes",
    "[renderingpass][resource-port][subresource][wp209b]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "compute_tasks": [{
            "name": "reduce_depth",
            "shader": "shaders/reduce_depth",
            "reads": ["depth_pyramid"],
            "writes": ["depth_pyramid"],
            "resource_ports": {
              "source_depth": {
                "resource": "depth_pyramid",
                "access": "sampled",
                "subresource": {
                  "mip": 2,
                  "layer": 1
                }
              },
              "reduced_depth": {
                "resource": "depth_pyramid",
                "access": "storage",
                "subresource": {
                  "mip": 3,
                  "layer": 1
                }
              }
            }
          }]
        })json");
    const auto tasks =
        parseComputeTaskDefinitionsFromConfigJson(
            config);
    REQUIRE(tasks.size() == 1);
    REQUIRE(
        tasks.front().resource_ports.size() ==
        2);
    const auto source = std::find_if(
        tasks.front().resource_ports.begin(),
        tasks.front().resource_ports.end(),
        [](const auto &port) {
            return port.name == "source_depth";
        });
    REQUIRE(
        source !=
        tasks.front().resource_ports.end());
    REQUIRE(source->subresource.has_value());
    REQUIRE(
        source->subresource->base_mip_level ==
        2);
    REQUIRE(
        source->subresource->base_array_layer ==
        1);

    auto overlap = config;
    overlap["compute_tasks"][0]
           ["resource_ports"]["reduced_depth"]
           ["subresource"]["mip"] = 2;
    REQUIRE_THROWS_WITH(
        parseComputeTaskDefinitionsFromConfigJson(
            overlap),
        Catch::Matchers::ContainsSubstring(
            "overlapping shader read/write subresources"));

    auto implicit = config;
    implicit["compute_tasks"][0]
            ["resource_ports"]["source_depth"]
            .erase("subresource");
    REQUIRE_THROWS_WITH(
        parseComputeTaskDefinitionsFromConfigJson(
            implicit),
        Catch::Matchers::ContainsSubstring(
            "only through an explicit subresource"));
}

TEST_CASE(
    "compute task schedules distinguish frame and logical-view work",
    "[renderingpass][compute][schedule][xr][wp210]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "compute_tasks": [
            {
              "name": "once",
              "shader": "shaders/once"
            },
            {
              "name": "per_eye",
              "shader": "shaders/per_eye",
              "schedule": "per_view"
            }
          ]
        })json");

    const auto tasks =
        parseComputeTaskDefinitionsFromConfigJson(
            config);
    REQUIRE(tasks.size() == 2);
    REQUIRE(
        tasks[0].schedule ==
        ComputeTaskSchedule::per_frame);
    REQUIRE(
        tasks[1].schedule ==
        ComputeTaskSchedule::per_view);
    REQUIRE(
        std::string{
            computeTaskScheduleName(
                tasks[0].schedule)} ==
        "per_frame");
    REQUIRE(
        std::string{
            computeTaskScheduleName(
                tasks[1].schedule)} ==
        "per_view");

    auto invalid = config;
    invalid["compute_tasks"][1]["schedule"] =
        "per_pass";
    REQUIRE_THROWS_WITH(
        parseComputeTaskDefinitionsFromConfigJson(
            invalid),
        Catch::Matchers::ContainsSubstring(
            "per_frame or per_view"));
}

TEST_CASE(
    "typed lighting buffers derive size from render extent and parse compute ports",
    "[renderingpass][resource-port][clustered][wp208]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "render_targets": [
            {
              "name": "display",
              "width": 1280,
              "height": 720
            },
            {
              "name": "lit_color",
              "extent_scale": 0.5
            }
          ],
          "buffers": [
            {
              "name": "light_inventory",
              "size": 65568,
              "host_source": "scene_lights_v2"
            },
            {
              "name": "light_selection",
              "size_from_extent": {
                "resource": "lit_color",
                "tile_width": 32,
                "tile_height": 32,
                "header_bytes": 32,
                "bytes_per_tile": 260
              }
            }
          ],
          "compute_tasks": [
            {
              "name": "select_lights",
              "shader": "shaders/select_lights",
              "reads": ["light_inventory"],
              "writes": ["light_selection"],
              "resource_ports": {
                "inventory": {
                  "resource": "light_inventory",
                  "kind": "buffer",
                  "element": "uvec4",
                  "access": "storage"
                },
                "selection": {
                  "resource": "light_selection",
                  "kind": "buffer",
                  "element": "uint",
                  "access": "storage"
                }
              }
            }
          ]
        })json");

    const auto buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            config);
    REQUIRE(buffers.size() == 2);
    REQUIRE(
        buffers[0].host_source ==
        FrameGraphHostBufferSource::
            scene_lights_v2);
    REQUIRE(buffers[0].size == 65568);
    REQUIRE(buffers[1].extent_size.has_value());
    REQUIRE(
        buffers[1].extent_size->resource ==
        "lit_color");
    REQUIRE(
        buffers[1].size ==
        32u + 20u * 12u * 260u);

    const auto tasks =
        parseComputeTaskDefinitionsFromConfigJson(
            config);
    REQUIRE(tasks.size() == 1);
    REQUIRE(tasks[0].resource_ports.size() == 2);
    REQUIRE(
        tasks[0].resource_ports[0].kind ==
        ShaderResourcePortKind::buffer);
    REQUIRE(
        tasks[0].resource_ports[0]
            .buffer_element ==
        ShaderResourceBufferElement::uvec4);
    REQUIRE(
        tasks[0].resource_ports[1]
            .buffer_element ==
        ShaderResourceBufferElement::
            unsigned_integer);

    auto invalid = config;
    invalid["buffers"][1]["size"] = 16;
    REQUIRE_THROWS_WITH(
        parseFrameGraphBufferDefinitionsFromJson(
            invalid),
        Catch::Matchers::ContainsSubstring(
            "cannot declare both size and size_from_extent"));
}

TEST_CASE(
    "typed indirect compute dispatch creates a graph read and validates its command buffer",
    "[renderingpass][compute][indirect][wp210]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "buffers": [
            {
              "name": "dispatch_arguments",
              "size": 24,
              "command_layout": "compute_dispatch"
            },
            {
              "name": "result",
              "size": 4
            }
          ],
          "compute_tasks": [
            {
              "name": "build_dispatch",
              "shader": "shaders/build_dispatch",
              "writes": ["dispatch_arguments"],
              "dispatch": {"groups": [1, 1, 1]}
            },
            {
              "name": "consume_dispatch",
              "shader": "shaders/consume_dispatch",
              "writes": ["result"],
              "dispatch": {
                "indirect": {
                  "buffer": "dispatch_arguments",
                  "offset": 12
                }
              }
            }
          ]
        })json");

    const auto buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            config);
    const auto tasks =
        parseComputeTaskDefinitionsFromConfigJson(
            config);
    REQUIRE(
        buffers.at(0).command_layout ==
        FrameGraphBufferCommandLayout::
            compute_dispatch);
    REQUIRE(
        std::string{
            frameGraphBufferCommandLayoutName(
                *buffers.at(0).command_layout)} ==
        "compute_dispatch");
    REQUIRE(tasks.at(1).dispatch.indirect.has_value());
    REQUIRE(
        tasks.at(1).dispatch.indirect->buffer ==
        "dispatch_arguments");
    REQUIRE(
        tasks.at(1).dispatch.indirect->offset == 12);
    REQUIRE_NOTHROW(
        validateComputeTaskBufferContracts(
            buffers, tasks));

    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            config);
    REQUIRE(graphs.size() == 1);
    const auto consumer = std::find_if(
        graphs.front().nodes.begin(),
        graphs.front().nodes.end(),
        [](const FrameGraphNodeDefinition &node) {
            return node.name ==
                   "consume_dispatch";
        });
    REQUIRE(consumer != graphs.front().nodes.end());
    REQUIRE(
        std::find(
            consumer->reads.begin(),
            consumer->reads.end(),
            "dispatch_arguments") !=
        consumer->reads.end());
    const auto plan =
        planFrameGraph(graphs.front());
    REQUIRE(
        framePlanOrder(plan) ==
        std::vector<std::string>{
            "build_dispatch",
            "consume_dispatch"});
    REQUIRE(plan.barriers.size() == 1);
    REQUIRE(
        plan.barriers.front().resource ==
        "dispatch_arguments");
    REQUIRE(
        plan.barriers.front().from ==
        "build_dispatch");
    REQUIRE(
        plan.barriers.front().to ==
        "consume_dispatch");

    auto mixed = config;
    mixed["compute_tasks"][1]["dispatch"]["groups"] =
        nlohmann::json::array({1, 1, 1});
    REQUIRE_THROWS_WITH(
        parseComputeTaskDefinitionsFromConfigJson(
            mixed),
        Catch::Matchers::ContainsSubstring(
            "cannot be combined"));

    auto unaligned = config;
    unaligned["compute_tasks"][1]
             ["dispatch"]["indirect"]["offset"] =
        2;
    REQUIRE_THROWS_WITH(
        parseComputeTaskDefinitionsFromConfigJson(
            unaligned),
        Catch::Matchers::ContainsSubstring(
            "4-byte aligned"));

    auto wrong_layout = config;
    wrong_layout["buffers"][0].erase(
        "command_layout");
    const auto untyped_buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            wrong_layout);
    REQUIRE_THROWS_WITH(
        validateComputeTaskBufferContracts(
            untyped_buffers, tasks),
        Catch::Matchers::ContainsSubstring(
            "requires command_layout"));

    auto unknown_buffer_tasks = tasks;
    unknown_buffer_tasks.at(1)
        .dispatch.indirect->buffer =
        "missing_arguments";
    REQUIRE_THROWS_WITH(
        validateComputeTaskBufferContracts(
            buffers, unknown_buffer_tasks),
        Catch::Matchers::ContainsSubstring(
            "unknown buffer"));

    auto self_writing_tasks = tasks;
    self_writing_tasks.at(1).writes.push_back(
        "dispatch_arguments");
    REQUIRE_THROWS_WITH(
        validateComputeTaskBufferContracts(
            buffers, self_writing_tasks),
        Catch::Matchers::ContainsSubstring(
            "separate producer task"));

    auto out_of_bounds = config;
    out_of_bounds["compute_tasks"][1]
                 ["dispatch"]["indirect"]["offset"] =
        16;
    const auto out_of_bounds_tasks =
        parseComputeTaskDefinitionsFromConfigJson(
            out_of_bounds);
    REQUIRE_THROWS_WITH(
        validateComputeTaskBufferContracts(
            buffers, out_of_bounds_tasks),
        Catch::Matchers::ContainsSubstring(
            "exceeds buffer"));
}

TEST_CASE(
    "typed GPU draw source creates render dependencies and validates fixed-state command buffers",
    "[renderingpass][indirect][draw][wp210]") {
    const auto config =
        nlohmann::json::parse(R"json({
          "buffers": [
            {
              "name": "candidate_draws",
              "size": 40,
              "host_source": "scene_draw_commands_v1",
              "command_layout": "indexed_draw"
            },
            {
              "name": "candidate_bounds",
              "size": 64,
              "host_source": "scene_draw_bounds_v1"
            },
            {
              "name": "visible_draws",
              "size": 40,
              "command_layout": "indexed_draw"
            },
            {
              "name": "visible_draw_count",
              "size": 4,
              "command_layout": "draw_count"
            }
          ],
          "compute_tasks": [
            {
              "name": "build_visible_draws",
              "shader": "shaders/build_visible_draws",
              "reads": ["candidate_draws", "candidate_bounds"],
              "writes": ["visible_draws", "visible_draw_count"],
              "before": ["geometry"],
              "dispatch": {"groups": [1, 1, 1]}
            }
          ],
          "rendering_passes": [
            {
              "name": "main",
              "passes": [
                {
                  "name": "geometry",
                  "type": "material",
                  "material_range": {"start": 0, "count": 1},
                  "gpu_draw_source": {
                    "commands": "visible_draws",
                    "count": "visible_draw_count",
                    "max_draw_count": 2
                  },
                  "output": {
                    "color": "swapchain",
                    "depth": null
                  }
                }
              ]
            }
          ]
        })json");

    const auto buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            config);
    REQUIRE(
        buffers.at(0).host_source ==
        FrameGraphHostBufferSource::
            scene_draw_commands_v1);
    REQUIRE(
        buffers.at(0).command_layout ==
        FrameGraphBufferCommandLayout::
            indexed_draw);
    REQUIRE(
        buffers.at(1).host_source ==
        FrameGraphHostBufferSource::
            scene_draw_bounds_v1);
    REQUIRE(
        std::string{
            frameGraphHostBufferSourceName(
                *buffers.at(1).host_source)} ==
        "scene_draw_bounds_v1");
    REQUIRE(
        std::string{
            frameGraphBufferCommandLayoutName(
                *buffers.at(3).command_layout)} ==
        "draw_count");
    REQUIRE_NOTHROW(
        validateGpuDrawSourceBufferContracts(
            config, buffers));

    const auto source =
        parseGpuDrawSourceFromJson(
            config["rendering_passes"][0]
                  ["passes"][0],
            "geometry");
    REQUIRE(source.has_value());
    REQUIRE(source->commands == "visible_draws");
    REQUIRE(source->count == "visible_draw_count");
    REQUIRE(source->max_draw_count == 2);
    REQUIRE(source->command_offset == 0);
    REQUIRE(source->count_offset == 0);

    const auto graphs =
        parseFrameGraphDefinitionsFromConfigJson(
            config);
    REQUIRE(graphs.size() == 1);
    const auto plan =
        planFrameGraph(graphs.front());
    REQUIRE(
        framePlanOrder(plan) ==
        std::vector<std::string>{
            "build_visible_draws", "geometry"});
    REQUIRE(plan.barriers.size() == 2);
    REQUIRE(
        std::any_of(
            plan.barriers.begin(),
            plan.barriers.end(),
            [](const auto &barrier) {
                return barrier.resource ==
                       "visible_draws";
            }));
    REQUIRE(
        std::any_of(
            plan.barriers.begin(),
            plan.barriers.end(),
            [](const auto &barrier) {
                return barrier.resource ==
                       "visible_draw_count";
            }));

    auto wrong_layout = config;
    wrong_layout["buffers"][2]
                ["command_layout"] =
        "draw_count";
    const auto wrong_layout_buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            wrong_layout);
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(
            wrong_layout,
            wrong_layout_buffers),
        Catch::Matchers::ContainsSubstring(
            "requires command_layout 'indexed_draw'"));

    auto too_small = config;
    too_small["buffers"][2]["size"] = 20;
    const auto too_small_buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            too_small);
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(
            too_small, too_small_buffers),
        Catch::Matchers::ContainsSubstring(
            "command range exceeds"));

    auto unaligned = config;
    unaligned["rendering_passes"][0]["passes"][0]
             ["gpu_draw_source"]["count_offset"] =
        2;
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(
            unaligned, buffers),
        Catch::Matchers::ContainsSubstring(
            "4-byte aligned"));

    auto multiple_ranges = config;
    multiple_ranges["rendering_passes"][0]["passes"][0]
                   ["material_range"]["count"] =
        2;
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(
            multiple_ranges, buffers),
        Catch::Matchers::ContainsSubstring(
            "material_range.count = 1"));

    auto wrong_pass = config;
    wrong_pass["rendering_passes"][0]["passes"][0]
              ["type"] =
        "fullscreen";
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(
            wrong_pass, buffers),
        Catch::Matchers::ContainsSubstring(
            "Only material passes"));

    auto invalid_host_source = config;
    invalid_host_source["buffers"][0]
                       ["command_layout"] =
        "draw_count";
    REQUIRE_THROWS_WITH(
        parseFrameGraphBufferDefinitionsFromJson(
            invalid_host_source),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_commands_v1"));

    auto invalid_bounds_size = config;
    invalid_bounds_size["buffers"][1]["size"] =
        40;
    REQUIRE_THROWS_WITH(
        parseFrameGraphBufferDefinitionsFromJson(
            invalid_bounds_size),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_bounds_v1"));

    auto invalid_bounds_layout = config;
    invalid_bounds_layout["buffers"][1]
                         ["command_layout"] =
        "indexed_draw";
    REQUIRE_THROWS_WITH(
        parseFrameGraphBufferDefinitionsFromJson(
            invalid_bounds_layout),
        Catch::Matchers::ContainsSubstring(
            "must not declare"));

    auto segmented = config;
    segmented["buffers"].push_back({
        {"name", "draw_segments"},
        {"size", 128},
        {"host_source",
         "scene_draw_segments_v1"},
    });
    segmented["rendering_passes"][0]["passes"][0]
             ["material_range"]["count"] = 2;
    auto &segmented_source =
        segmented["rendering_passes"][0]
                 ["passes"][0]
                 ["gpu_draw_source"];
    segmented_source["layout"] =
        "draw_queue_segments_v1";
    segmented_source["segments"] =
        "draw_segments";
    const auto segmented_buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            segmented);
    REQUIRE_NOTHROW(
        validateGpuDrawSourceBufferContracts(
            segmented,
            segmented_buffers));
    const auto parsed_segmented =
        parseGpuDrawSourceFromJson(
            segmented["rendering_passes"][0]
                     ["passes"][0],
            "segmented test");
    REQUIRE(parsed_segmented.has_value());
    CHECK(
        parsed_segmented->layout ==
        GpuDrawSourceLayout::
            draw_queue_segments_v1);
    CHECK(
        std::string{
            frameGraphHostBufferSourceName(
                *segmented_buffers.back()
                     .host_source)} ==
        "scene_draw_segments_v1");

    auto wrong_segment_source = segmented;
    wrong_segment_source["buffers"].back()
                        .erase("host_source");
    const auto wrong_segment_buffers =
        parseFrameGraphBufferDefinitionsFromJson(
            wrong_segment_source);
    REQUIRE_THROWS_WITH(
        validateGpuDrawSourceBufferContracts(
            wrong_segment_source,
            wrong_segment_buffers),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_segments_v1"));

    auto invalid_segment_size = segmented;
    invalid_segment_size["buffers"].back()
                        ["size"] = 40;
    REQUIRE_THROWS_WITH(
        parseFrameGraphBufferDefinitionsFromJson(
            invalid_segment_size),
        Catch::Matchers::ContainsSubstring(
            "scene_draw_segments_v1"));

    auto invalid_segment_layout = segmented;
    invalid_segment_layout["buffers"].back()
                          ["command_layout"] =
        "indexed_draw";
    REQUIRE_THROWS_WITH(
        parseFrameGraphBufferDefinitionsFromJson(
            invalid_segment_layout),
        Catch::Matchers::ContainsSubstring(
            "must not declare"));

    auto missing_segments = segmented;
    missing_segments["rendering_passes"][0]
                    ["passes"][0]
                    ["gpu_draw_source"]
                    .erase("segments");
    REQUIRE_THROWS_WITH(
        parseGpuDrawSourceFromJson(
            missing_segments
                ["rendering_passes"][0]
                ["passes"][0],
            "segmented test"),
        Catch::Matchers::ContainsSubstring(
            "requires segments"));
}

TEST_CASE("fullscreen pass JSON parser rejects explicit shader files and names the stem form", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "fullscreen.vert.spv"}, {"fragment", "lighting"}}},
    };

    std::string message;
    try {
        (void)parseFullscreenPassInfoFromJson(pass_json, "lighting_pass");
    } catch (const std::exception &ex) {
        message = ex.what();
    }
    REQUIRE(message.find("fullscreen.vert.spv") != std::string::npos);
    REQUIRE(message.find("extensionless") != std::string::npos);
    REQUIRE(message.find("vertex") != std::string::npos);
}

TEST_CASE("fullscreen pass JSON parser does not infer options from pass name", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}},
    };

    const auto info = parseFullscreenPassInfoFromJson(pass_json, "lighting_pass");

    REQUIRE(info.push_constants == FullscreenPushConstantData::eNone);
    REQUIRE_FALSE(info.uses_light_data);
}

TEST_CASE("pass info JSON parser reads shadow depth shader option", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "shadow_depth";
    parsePassTypeFromJson(pass_def, nlohmann::json{{"type", "shadow_depth"}});

    parseShadowDepthPassInfoIntoDefinition(pass_def, nlohmann::json::object());

    REQUIRE(pass_def.isShadowDepth());
    REQUIRE(pass_def.shadowDepthInfo().vert_shader.ref == "engine://shadow_depth");
    REQUIRE(pass_def.shadowDepthInfo().vert_shader.kind == ShaderReferenceKind::stem);

    parseShadowDepthPassInfoIntoDefinition(
        pass_def,
        nlohmann::json{{"shader", {{"vertex", "shaders/custom_shadow"}}}});

    REQUIRE(pass_def.shadowDepthInfo().vert_shader.ref == "shaders/custom_shadow");
}

TEST_CASE("fullscreen pass JSON parser rejects deprecated projection matrix flag", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"needs_projection_matrix", true},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "ssao"}}},
    };

    REQUIRE_THROWS_AS(parseFullscreenPassInfoFromJson(pass_json, "ssao_pass"), std::runtime_error);
}

TEST_CASE("pass info JSON parser reads pass type and applies UI defaults", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "ui";

    parsePassTypeFromJson(pass_def, nlohmann::json{{"type", "ui"}});

    REQUIRE(pass_def.isUi());
    REQUIRE(pass_def.color_load_op == vk::AttachmentLoadOp::eLoad);
}

TEST_CASE("pass info JSON parser applies fullscreen info only to fullscreen passes", "[renderingpass]") {
    PassDefinition fullscreen_pass;
    fullscreen_pass.name = "debug_texture";
    parsePassTypeFromJson(fullscreen_pass, nlohmann::json{{"type", "fullscreen"}});

    const nlohmann::json fullscreen_json{
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "debug_texture"}}},
        {"push_constants", "projection_view"},
    };
    parseFullscreenPassInfoIntoDefinition(fullscreen_pass, fullscreen_json);

    REQUIRE(fullscreen_pass.fullscreenInfo().frag_shader.ref == "debug_texture");
    REQUIRE(fullscreen_pass.fullscreenInfo().push_constants == FullscreenPushConstantData::eProjectionView);

    PassDefinition material_pass;
    material_pass.name = "geometry";
    material_pass.pass_info = MaterialPassInfo{};
    parseFullscreenPassInfoIntoDefinition(material_pass, nlohmann::json::object());

    REQUIRE(material_pass.isMaterial());
}

TEST_CASE("material pass selects an opaque named variant through an explicit bounded route",
          "[renderingpass][material-variant][wp206b]") {
    PassDefinition pass;
    pass.name = "silhouette_overlay";
    pass.pass_info = MaterialPassInfo{};
    const auto authored = nlohmann::json{
        {"material_contract", "forward_opaque_v1"},
        {"material_filter",
         {{"include", {"outlined"}}}},
        {"material_variant", "silhouette"},
    };
    parseMaterialPassInfoFromJson(pass, authored);
    REQUIRE(pass.materialInfo().material_variant ==
            "silhouette");
    REQUIRE(pass.materialInfo().contract ==
            MaterialPassContract::forward_opaque_v1);
    REQUIRE(pass.materialInfo().material_filter
                ->include ==
            std::vector<std::string>{"outlined"});

    auto invalid = authored;
    invalid.erase("material_contract");
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(pass, invalid),
        Catch::Matchers::ContainsSubstring(
            "requires explicit material_contract"));

    invalid = authored;
    invalid["material_filter"]["include"] =
        nlohmann::json::array();
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(pass, invalid),
        Catch::Matchers::ContainsSubstring(
            "requires non-empty material_filter.include"));

    invalid = authored;
    invalid["material_variant"] = "bad/name";
    REQUIRE_THROWS_AS(
        parseMaterialPassInfoFromJson(pass, invalid),
        std::runtime_error);
}

TEST_CASE("rendering pass target JSON parser applies output and input targets", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "color_target") {
            return GlobalRenderTargetId{3};
        }
        if (name == "depth_target") {
            return GlobalRenderTargetId{4};
        }
        if (name == "input_target") {
            return GlobalRenderTargetId{5};
        }
        return noRenderTargetId();
    }};

    PassDefinition pass_def;
    pass_def.name = "postprocess";

    const nlohmann::json pass_json{
        {"output", {{"color", "color_target"}, {"depth", "depth_target"}}},
        {"input", "input_target"},
    };

    parsePassOutputTargetsFromJson(pass_def, resolver, pass_json);
    parsePassInputTargetsFromJson(pass_def, resolver, pass_json);

    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(pass_def.output_color[0] == GlobalRenderTargetId{3});
    REQUIRE(pass_def.output_depth == GlobalRenderTargetId{4});
    REQUIRE(pass_def.input_targets.size() == 1);
    REQUIRE(pass_def.input_targets[0] == GlobalRenderTargetId{5});
}

TEST_CASE("rendering pass target JSON parser handles swapchain and omitted input", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};

    PassDefinition pass_def;
    pass_def.name = "present";

    const nlohmann::json pass_json{
        {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
    };

    parsePassOutputTargetsFromJson(pass_def, resolver, pass_json);
    parsePassInputTargetsFromJson(pass_def, resolver, pass_json);

    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(isSwapchainRenderTarget(pass_def.output_color[0]));
    REQUIRE(pass_def.output_depth == noRenderTargetId());
    REQUIRE(pass_def.input_targets.empty());
}

TEST_CASE("rendering pass target JSON parser rejects malformed pass outputs", "[renderingpass]") {
    const auto resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};

    PassDefinition pass_def;
    pass_def.name = "bad_pass";

    REQUIRE_THROWS_AS(parsePassOutputTargetsFromJson(pass_def, resolver, nlohmann::json::object()),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parsePassOutputTargetsFromJson(pass_def, resolver, nlohmann::json{{"output", 1}}),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parsePassOutputTargetsFromJson(pass_def, resolver,
                                                     nlohmann::json{{"output", {{"color", "swapchain"}}}}),
                      std::runtime_error);
}

TEST_CASE("pass definition JSON parser builds a fullscreen pass definition", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "half_color") {
            return GlobalRenderTargetId{3};
        }
        return noRenderTargetId();
    }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId id) {
        if (id == GlobalRenderTargetId{3}) {
            return RenderTargetMetadata{"half_color", vk::ImageUsageFlagBits::eColorAttachment |
                                                          vk::ImageUsageFlagBits::eSampled,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    const nlohmann::json pass_json{
        {"name", "debug_texture"},
        {"type", "fullscreen"},
        {"output", {{"color", "half_color"}, {"depth", nullptr}}},
        {"shader", {{"vertex", "fullscreen"}, {"fragment", "debug_texture"}}},
        {"implementation", {{"provider", "fixture.fullscreen"}}},
        {"regions",
         nlohmann::json::array(
             {"region.post.fixture", "region.post"})},
        {"push_constants", "projection_view"},
        {"clear_color", nlohmann::json::array({0.0f, 0.0f, 0.0f, 1.0f})},
    };

    const auto pass_def = parsePassDefinitionFromJson(pass_json, name_resolver, metadata_resolver);

    REQUIRE(pass_def.name == "debug_texture");
    REQUIRE(pass_def.isFullscreen());
    REQUIRE(pass_def.output_color.size() == 1);
    REQUIRE(pass_def.output_color[0] == GlobalRenderTargetId{3});
    REQUIRE(pass_def.output_depth == noRenderTargetId());
    REQUIRE(pass_def.fullscreenInfo().frag_shader.ref == "debug_texture");
    REQUIRE(pass_def.fullscreenInfo().push_constants == FullscreenPushConstantData::eProjectionView);
    REQUIRE(
        pass_def.requested_implementation_provider ==
        std::optional<std::string>{"fixture.fullscreen"});
    REQUIRE_FALSE(pass_def.implementation_selection.has_value());
    REQUIRE(
        pass_def.region_tags ==
        std::vector<std::string>{
            "region.post.fixture", "region.post"});
}

TEST_CASE(
    "pass definition JSON parser keeps implementation selection typed and fullscreen-only",
    "[renderingpass][render-pass][wp200]") {
    const auto name_resolver =
        RenderTargetNameResolver{
            [](const std::string &name) {
                return name == "half_color"
                           ? GlobalRenderTargetId{3}
                           : noRenderTargetId();
            }};
    const auto metadata_resolver =
        RenderTargetMetadataResolver{
            [](GlobalRenderTargetId id) {
                if (id != GlobalRenderTargetId{3}) {
                    throw std::runtime_error(
                        "unexpected render target metadata lookup");
                }
                return RenderTargetMetadata{
                    "half_color",
                    vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR8G8B8A8Unorm,
                    vk::Extent2D{1280, 720}};
            }};
    const nlohmann::json material_json{
        {"name", "material"},
        {"type", "material"},
        {"implementation",
         {{"provider", "fixture.fullscreen"}}},
        {"output",
         {{"color", "half_color"},
          {"depth", nullptr}}},
    };
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            material_json, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "fullscreen passes only"));

    auto malformed = nlohmann::json{
        {"name", "fullscreen"},
        {"type", "fullscreen"},
        {"implementation",
         {{"provider", "fixture.fullscreen"},
          {"fallback", "builtin.fullscreen_v1"}}},
        {"output",
         {{"color", "half_color"},
          {"depth", nullptr}}},
        {"shader",
         {{"vertex", "fullscreen"},
          {"fragment", "composite"}}},
    };
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            malformed, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "containing only a provider string"));

    malformed["implementation"] =
        {{"provider", ""}};
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            malformed, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "empty or too long"));
    malformed["implementation"] = {
        {"provider",
         std::string(
             RenderPass::maximumProviderNameBytesV1 + 1,
             'x')}};
    REQUIRE_THROWS_WITH(
        parsePassDefinitionFromJson(
            malformed, name_resolver,
            metadata_resolver),
        Catch::Matchers::ContainsSubstring(
            "empty or too long"));
}

TEST_CASE("fullscreen pass JSON parser accepts shader stem references", "[renderingpass]") {
    const nlohmann::json pass_json{
        {"shader", {{"vertex", "shaders/fullscreen"}, {"fragment", "engine://bloom_blur_h"}}},
    };

    const auto info = parseFullscreenPassInfoFromJson(pass_json, "stem_pass");

    REQUIRE(info.vert_shader.ref == "shaders/fullscreen");
    REQUIRE(info.vert_shader.stage == ShaderStage::vertex);
    REQUIRE(info.vert_shader.kind == ShaderReferenceKind::stem);
    REQUIRE_FALSE(info.vert_shader.backend_specific);
    REQUIRE(info.frag_shader.ref == "engine://bloom_blur_h");
    REQUIRE(info.frag_shader.stage == ShaderStage::fragment);
    REQUIRE(info.frag_shader.kind == ShaderReferenceKind::stem);
}

TEST_CASE("pass definition JSON parser rejects invalid pass object", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId) -> RenderTargetMetadata {
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    REQUIRE_THROWS_AS(parsePassDefinitionFromJson(nlohmann::json::array(), name_resolver, metadata_resolver),
                      std::runtime_error);
}

TEST_CASE("pass sequence JSON parser validates produced input order", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "source_color") {
            return GlobalRenderTargetId{3};
        }
        if (name == "final_color") {
            return GlobalRenderTargetId{4};
        }
        return noRenderTargetId();
    }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId id) {
        if (id == GlobalRenderTargetId{3}) {
            return RenderTargetMetadata{"source_color", vk::ImageUsageFlagBits::eColorAttachment |
                                                            vk::ImageUsageFlagBits::eSampled,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        if (id == GlobalRenderTargetId{4}) {
            return RenderTargetMetadata{"final_color", vk::ImageUsageFlagBits::eColorAttachment,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    const nlohmann::json pass_set_json{
        {"passes",
         nlohmann::json::array({
             {{"name", "source"}, {"type", "fullscreen"},
              {"output", {{"color", "source_color"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "source"}}}},
             {{"name", "composite"}, {"type", "fullscreen"}, {"input", "source_color"},
              {"output", {{"color", "final_color"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "composite"}}}},
         })},
    };

    const auto passes = parsePassSequenceFromJson(pass_set_json, "main", name_resolver, metadata_resolver);

    REQUIRE(passes.size() == 2);
    REQUIRE(passes[0].name == "source");
    REQUIRE(passes[1].input_targets.size() == 1);
    REQUIRE(passes[1].input_targets[0] == GlobalRenderTargetId{3});
}

TEST_CASE("pass sequence JSON parser allows depth output as later input", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &name) {
        if (name == "shadow_map") {
            return GlobalRenderTargetId{3};
        }
        if (name == "lit_color") {
            return GlobalRenderTargetId{4};
        }
        return noRenderTargetId();
    }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId id) {
        if (id == GlobalRenderTargetId{3}) {
            return RenderTargetMetadata{"shadow_map", vk::ImageUsageFlagBits::eDepthStencilAttachment |
                                                          vk::ImageUsageFlagBits::eSampled,
                                        vk::Format::eD32Sfloat, vk::Extent2D{2048, 2048}};
        }
        if (id == GlobalRenderTargetId{4}) {
            return RenderTargetMetadata{"lit_color", vk::ImageUsageFlagBits::eColorAttachment,
                                        vk::Format::eR8G8B8A8Unorm, vk::Extent2D{1280, 720}};
        }
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    const nlohmann::json pass_set_json{
        {"passes",
         nlohmann::json::array({
             {{"name", "shadow_depth"}, {"type", "shadow_depth"},
              {"output", {{"color", nullptr}, {"depth", "shadow_map"}}}},
             {{"name", "lighting_pass"}, {"type", "fullscreen"}, {"input", "shadow_map"},
              {"output", {{"color", "lit_color"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "lighting"}}}},
         })},
    };

    const auto passes = parsePassSequenceFromJson(pass_set_json, "main", name_resolver, metadata_resolver);

    REQUIRE(passes.size() == 2);
    REQUIRE(passes[0].isShadowDepth());
    REQUIRE(passes[1].input_targets == std::vector<GlobalRenderTargetId>{GlobalRenderTargetId{3}});
}

TEST_CASE("pass sequence JSON parser rejects duplicate pass names", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId) -> RenderTargetMetadata {
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    const nlohmann::json pass_set_json{
        {"passes",
         nlohmann::json::array({
             {{"name", "same"}, {"type", "fullscreen"}, {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "one"}}}},
             {{"name", "same"}, {"type", "fullscreen"}, {"output", {{"color", "swapchain"}, {"depth", nullptr}}},
              {"shader", {{"vertex", "fullscreen"}, {"fragment", "two"}}}},
         })},
    };

    REQUIRE_THROWS_AS(parsePassSequenceFromJson(pass_set_json, "main", name_resolver, metadata_resolver),
                      std::runtime_error);
}

TEST_CASE("pass sequence JSON parser rejects malformed pass list", "[renderingpass]") {
    const auto name_resolver = RenderTargetNameResolver{[](const std::string &) { return noRenderTargetId(); }};
    const auto metadata_resolver = RenderTargetMetadataResolver{[](GlobalRenderTargetId) -> RenderTargetMetadata {
        throw std::runtime_error("unexpected render target metadata lookup");
    }};

    REQUIRE_THROWS_AS(parsePassSequenceFromJson(nlohmann::json::object(), "main", name_resolver, metadata_resolver),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parsePassSequenceFromJson(nlohmann::json{{"passes", 1}}, "main", name_resolver,
                                                metadata_resolver),
                      std::runtime_error);
}

TEST_CASE("pass attachment options parser applies explicit color attachment options", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "bloom_composite";

    const nlohmann::json pass_json{
        {"clear_color", nlohmann::json::array({0.25f, 0.5f, 0.75f, 1.0f})},
        {"color_load_op", "Load"},
        {"color_store_op", "DontCare"},
        {"depth_load_op", "Clear"},
        {"depth_store_op", "Store"},
    };

    parsePassAttachmentOptionsFromJson(pass_def, pass_json);

    REQUIRE(pass_def.clear_color[0] == 0.25);
    REQUIRE(pass_def.clear_color[1] == 0.5);
    REQUIRE(pass_def.clear_color[2] == 0.75);
    REQUIRE(pass_def.clear_color[3] == 1.0);
    REQUIRE(pass_def.color_load_op == vk::AttachmentLoadOp::eLoad);
    REQUIRE(pass_def.color_store_op == vk::AttachmentStoreOp::eDontCare);
    REQUIRE(pass_def.depth_load_op == vk::AttachmentLoadOp::eClear);
    REQUIRE(pass_def.depth_store_op == vk::AttachmentStoreOp::eStore);
}

TEST_CASE(
    "pass attachment options apply sparse per-target clear colors",
    "[renderingpass][typed-clear][wp218]") {
    PassDefinition pass_def;
    pass_def.name = "extended_geometry";
    pass_def.output_color = {
        GlobalRenderTargetId{0},
        GlobalRenderTargetId{1},
    };
    const nlohmann::json pass_json{
        {"output",
         {{"color",
           nlohmann::json::array(
               {"albedo", "object_id"})},
          {"depth", "depth"}}},
        {"clear_color", {0.0, 0.0, 0.0, 1.0}},
        {"clear_colors",
         {{"object_id",
           {4294967295.0, 0.0, 0.0, 0.0}}}},
    };

    parsePassAttachmentOptionsFromJson(
        pass_def, pass_json);
    REQUIRE(
        pass_def.logicalColorClearValue(0) ==
        std::array<double, 4>{
            0.0, 0.0, 0.0, 1.0});
    REQUIRE(
        pass_def.logicalColorClearValue(1) ==
        std::array<double, 4>{
            4294967295.0, 0.0, 0.0, 0.0});

    auto invalid = pass_json;
    invalid["clear_colors"] = {
        {"not_an_output", {0, 0, 0, 0}}};
    REQUIRE_THROWS_WITH(
        parsePassAttachmentOptionsFromJson(
            pass_def, invalid),
        Catch::Matchers::ContainsSubstring(
            "non-output render target"));
}

TEST_CASE("pass attachment options parser preserves defaults when fields are omitted", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";

    parsePassAttachmentOptionsFromJson(pass_def, nlohmann::json::object());

    REQUIRE(pass_def.clear_color[0] == 0.0);
    REQUIRE(pass_def.clear_color[1] == 0.0);
    REQUIRE(pass_def.clear_color[2] == 0.0);
    REQUIRE(pass_def.clear_color[3] == 1.0);
    REQUIRE(pass_def.color_load_op == vk::AttachmentLoadOp::eClear);
    REQUIRE(pass_def.color_store_op == vk::AttachmentStoreOp::eStore);
}

TEST_CASE("material pass info parser applies explicit material range", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";
    pass_def.pass_info = MaterialPassInfo{};

    const nlohmann::json pass_json{
        {"material_range", {{"start", 3}, {"count", 7}}},
    };

    parseMaterialPassInfoFromJson(pass_def, pass_json);

    REQUIRE(pass_def.materialInfo().material_start == 3);
    REQUIRE(pass_def.materialInfo().material_count == 7);
}

TEST_CASE("material pass info parser compiles stable include and exclude tags",
          "[renderingpass][draw-tag][wp206a]") {
    PassDefinition pass_def;
    pass_def.name = "outline";
    pass_def.pass_info = MaterialPassInfo{};

    parseMaterialPassInfoFromJson(
        pass_def,
        nlohmann::json{
            {"material_filter",
             {{"include",
               {"outline", "character"}},
              {"exclude", {"hidden"}}}},
        });

    REQUIRE(
        pass_def.materialInfo()
            .material_filter.has_value());
    const auto &filter =
        *pass_def.materialInfo().material_filter;
    REQUIRE(filter.include ==
            std::vector<std::string>{
                "character", "outline"});
    REQUIRE(filter.exclude ==
            std::vector<std::string>{"hidden"});
    REQUIRE(
        filter.id ==
        materialDrawTagFilterId(
            filter.include, filter.exclude));

    auto invalid = nlohmann::json{
        {"material_filter",
         {{"include", {""}}}},
    };
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(
            pass_def, invalid),
        Catch::Matchers::ContainsSubstring(
            "tag is empty or too long"));

    invalid = {
        {"material_filter",
         {{"include", {"outline", "outline"}}}},
    };
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(
            pass_def, invalid),
        Catch::Matchers::ContainsSubstring(
            "duplicate tag: outline"));

    invalid = {
        {"material_filter",
         {{"include", {"outline"}},
          {"exclude", {"outline"}}}},
    };
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(
            pass_def, invalid),
        Catch::Matchers::ContainsSubstring(
            "includes and excludes the same tag"));

    invalid = {
        {"material_filter",
         {{"any", {"outline"}}}},
    };
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(
            pass_def, invalid),
        Catch::Matchers::ContainsSubstring(
            "unknown field 'any'"));
}

TEST_CASE("material pass screen inputs resolve named typed targets and reject mismatches",
          "[renderingpass][material-screen-input][rpe6b1]") {
    const RenderTargetNameResolver names{[](const std::string &name) {
        if (name == "opaque_color") return GlobalRenderTargetId{10};
        if (name == "opaque_depth") return GlobalRenderTargetId{11};
        return noRenderTargetId();
    }};
    const RenderTargetMetadataResolver metadata{
        [](GlobalRenderTargetId target) {
            if (target == GlobalRenderTargetId{10}) {
                return RenderTargetMetadata{
                    "opaque_color",
                    vk::ImageUsageFlagBits::eTransferDst |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR16G16B16A16Sfloat,
                    vk::Extent2D{1280, 720}};
            }
            if (target == GlobalRenderTargetId{11}) {
                return RenderTargetMetadata{
                    "opaque_depth",
                    vk::ImageUsageFlagBits::eTransferDst |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eD32Sfloat, vk::Extent2D{1280, 720}};
            }
            throw std::runtime_error("unexpected target");
        }};

    PassDefinition pass;
    pass.name = "forward_transparent";
    pass.pass_info = MaterialPassInfo{};
    parseMaterialPassScreenInputsFromJson(
        pass,
        nlohmann::json{{"screen_inputs",
                        {{"opaque_color", "opaque_color"},
                         {"opaque_depth", "opaque_depth"},
                         {"scene_depth", "opaque_depth"},
                         {"linear_view_depth", "opaque_depth"}}}},
        names, metadata);
    REQUIRE(pass.materialInfo().screen_inputs.size() == 4);
    REQUIRE(pass.input_targets.size() == 2);
    REQUIRE(std::find(pass.input_targets.begin(), pass.input_targets.end(),
                      GlobalRenderTargetId{10}) != pass.input_targets.end());
    REQUIRE(std::find(pass.input_targets.begin(), pass.input_targets.end(),
                      GlobalRenderTargetId{11}) != pass.input_targets.end());
    const auto linear = std::find_if(
        pass.materialInfo().screen_inputs.begin(),
        pass.materialInfo().screen_inputs.end(), [](const auto &input) {
            return input.contract.name == "linear_view_depth";
        });
    REQUIRE(linear != pass.materialInfo().screen_inputs.end());
    REQUIRE(linear->contract.sampled_type ==
            linearViewDepthV1(makeBuiltinLogicalTypeRegistry()));
    REQUIRE(linear->contract.footprint.kind ==
            LogicalReadFootprintKind::same_pixel);
    REQUIRE(linear->contract.conversion ==
            std::optional<std::string>{"pelican.render.depth_linearize@1"});

    PassDefinition mismatch;
    mismatch.name = "bad_forward";
    mismatch.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassScreenInputsFromJson(
            mismatch,
            nlohmann::json{{"screen_inputs",
                            {{"opaque_color", "opaque_depth"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring("opaque_color") &&
            Catch::Matchers::ContainsSubstring("source type"));

    PassDefinition unknown;
    unknown.name = "unknown_forward";
    unknown.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassScreenInputsFromJson(
            unknown,
            nlohmann::json{{"screen_inputs",
                            {{"history_magic", "opaque_color"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring("history_magic"));
}

TEST_CASE("material pass surface resources resolve the feature-owned directional shadow contract",
          "[renderingpass][material-surface-resource][wp205]") {
    const RenderTargetNameResolver names{[](const std::string &name) {
        if (name == "shadow_map") return GlobalRenderTargetId{20};
        if (name == "scene_color") return GlobalRenderTargetId{21};
        return noRenderTargetId();
    }};
    const RenderTargetMetadataResolver metadata{
        [](GlobalRenderTargetId target) {
            if (target == GlobalRenderTargetId{20}) {
                return RenderTargetMetadata{
                    "shadow_map",
                    vk::ImageUsageFlagBits::eDepthStencilAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eD32Sfloat,
                    vk::Extent2D{2048, 2048}};
            }
            if (target == GlobalRenderTargetId{21}) {
                return RenderTargetMetadata{
                    "scene_color",
                    vk::ImageUsageFlagBits::eColorAttachment |
                        vk::ImageUsageFlagBits::eSampled,
                    vk::Format::eR16G16B16A16Sfloat,
                    vk::Extent2D{1280, 720}};
            }
            throw std::runtime_error("unexpected target");
        }};

    PassDefinition pass;
    pass.name = "forward_opaque";
    pass.pass_info = MaterialPassInfo{};
    parseMaterialPassSurfaceResourcesFromJson(
        pass,
        nlohmann::json{
            {"surface_resources",
             {{"directional_shadow", "shadow_map"}}}},
        names, metadata);

    REQUIRE(pass.materialInfo().surface_resources.size() == 1);
    const auto &binding =
        pass.materialInfo().surface_resources.front();
    REQUIRE(binding.target == GlobalRenderTargetId{20});
    REQUIRE(binding.contract.name == "directional_shadow");
    REQUIRE(binding.contract.source_type ==
            deviceDepthV1(makeBuiltinLogicalTypeRegistry()));
    REQUIRE(binding.contract.sampled_type ==
            deviceDepthV1(makeBuiltinLogicalTypeRegistry()));
    REQUIRE(binding.contract.footprint.kind ==
            LogicalReadFootprintKind::arbitrary);
    REQUIRE(binding.contract.sampling ==
            MaterialPassInputSampling::nearest_clamp_to_edge);
    REQUIRE(binding.contract.view_policy ==
            MaterialPassInputViewPolicy::shared_2d);
    REQUIRE(binding.contract.fallback ==
            MaterialPassInputFallback::fully_lit);
    REQUIRE(binding.contract.relation);
    REQUIRE(binding.contract.relation->light_index == 0);
    REQUIRE(binding.contract.relation->transform ==
            "pelican.light.shadow_view_projection@1");
    REQUIRE(pass.input_targets ==
            std::vector<GlobalRenderTargetId>{
                GlobalRenderTargetId{20}});

    PassDefinition wrong_type;
    wrong_type.name = "bad_forward";
    wrong_type.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassSurfaceResourcesFromJson(
            wrong_type,
            nlohmann::json{
                {"surface_resources",
                 {{"directional_shadow", "scene_color"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring(
            "directional_shadow") &&
            Catch::Matchers::ContainsSubstring("source type"));

    PassDefinition user_owned;
    user_owned.name = "bad_forward";
    user_owned.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassSurfaceResourcesFromJson(
            user_owned,
            nlohmann::json{
                {"surface_resources",
                 {{"opaque_depth", "shadow_map"}}}},
            names, metadata),
        Catch::Matchers::ContainsSubstring("opaque_depth") &&
            Catch::Matchers::ContainsSubstring("not feature-owned"));
}

TEST_CASE(
    "material pass resource ports resolve named image and buffer reads",
    "[renderingpass][material-resource][wp207b]") {
    const RenderTargetNameResolver names{
        [](const std::string &name) {
            if (name == "simulation_color") {
                return GlobalRenderTargetId{30};
            }
            if (name == "history_color") {
                return GlobalRenderTargetId{31};
            }
            return noRenderTargetId();
        }};
    const std::unordered_set<std::string> buffers{
        "deformed_positions"};

    PassDefinition pass;
    pass.name = "geometry";
    pass.pass_info = MaterialPassInfo{};
    parseMaterialPassResourcesFromJson(
        pass,
        nlohmann::json{
            {"material_resources",
             {
                 {"displacement",
                  {
                      {"resource",
                       "deformed_positions"},
                      {"access", "storage"},
                  }},
                 {"simulation_color",
                  {
                      {"resource",
                       "simulation_color"},
                      {"access", "sampled"},
                      {"view", "per_view"},
                      {"sampling",
                       {
                           {"filter", "nearest"},
                           {"address",
                            "clamp_to_edge"},
                       }},
                      {"footprint",
                       {
                           {"kind",
                            "neighborhood"},
                           {"radius", 2u},
                       }},
                  }},
                 {"previous_color",
                  {
                      {"resource",
                       "history_color@history"},
                      {"access", "sampled"},
                  }},
             }}},
        names, buffers);

    REQUIRE(
        pass.materialInfo().material_resources.size() ==
        3);
    REQUIRE(
        pass.input_buffers ==
        std::vector<std::string>{
            "deformed_positions"});
    REQUIRE(pass.input_targets.size() == 2);
    REQUIRE(
        pass.input_target_history.size() == 2);
    const auto current_target = std::find(
        pass.input_targets.begin(),
        pass.input_targets.end(),
        GlobalRenderTargetId{30});
    const auto history_target = std::find(
        pass.input_targets.begin(),
        pass.input_targets.end(),
        GlobalRenderTargetId{31});
    REQUIRE(current_target != pass.input_targets.end());
    REQUIRE(history_target != pass.input_targets.end());
    REQUIRE_FALSE(pass.input_target_history.at(
        static_cast<std::size_t>(std::distance(
            pass.input_targets.begin(),
            current_target))));
    REQUIRE(pass.input_target_history.at(
        static_cast<std::size_t>(std::distance(
            pass.input_targets.begin(),
            history_target))));

    const auto find_binding =
        [&](std::string_view name)
        -> const MaterialPassResourceBinding & {
        const auto found = std::find_if(
            pass.materialInfo()
                .material_resources.begin(),
            pass.materialInfo()
                .material_resources.end(),
            [&](const auto &binding) {
                return binding.port.name == name;
            });
        REQUIRE(
            found !=
            pass.materialInfo()
                .material_resources.end());
        return *found;
    };
    const auto &displacement =
        find_binding("displacement");
    REQUIRE(displacement.isBuffer());
    REQUIRE_FALSE(displacement.isImage());
    REQUIRE(
        displacement.port.access ==
        ShaderResourcePortAccess::storage);

    const auto &simulation =
        find_binding("simulation_color");
    REQUIRE(simulation.isImage());
    REQUIRE(
        simulation.port.view ==
        ShaderResourcePortView::per_view);
    REQUIRE(
        simulation.port.sampling.filter ==
        ShaderResourcePortFilter::nearest);
    REQUIRE(
        simulation.port.sampling.address_mode ==
        ShaderResourcePortAddressMode::
            clamp_to_edge);
    REQUIRE(
        simulation.footprint.kind ==
        LogicalReadFootprintKind::neighborhood);
    REQUIRE(simulation.footprint.radius == 2u);

    const auto &history =
        find_binding("previous_color");
    REQUIRE(history.isImage());
    REQUIRE(history.history);
    REQUIRE(
        history.footprint.kind ==
        LogicalReadFootprintKind::temporal);

    PassDefinition subresource;
    subresource.name = "subresource";
    subresource.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassResourcesFromJson(
            subresource,
            nlohmann::json{
                {"material_resources",
                 {{"pyramid",
                   {
                       {"resource",
                        "simulation_color"},
                       {"access", "sampled"},
                       {"subresource",
                        {{"mip", 1}}},
                   }}}}},
            names, buffers),
        Catch::Matchers::ContainsSubstring(
            "does not yet support image subresource views"));

    PassDefinition missing;
    missing.name = "missing";
    missing.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassResourcesFromJson(
            missing,
            nlohmann::json{
                {"material_resources",
                 {{"unknown", "missing_target"}}}},
            names, buffers),
        Catch::Matchers::ContainsSubstring(
            "resource not found"));

    PassDefinition buffer_history;
    buffer_history.name = "buffer_history";
    buffer_history.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassResourcesFromJson(
            buffer_history,
            nlohmann::json{
                {"material_resources",
                 {{"previous",
                   "deformed_positions@history"}}}},
            names, buffers),
        Catch::Matchers::ContainsSubstring(
            "buffer history is not supported"));

    PassDefinition mixed_history;
    mixed_history.name = "mixed_history";
    mixed_history.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassResourcesFromJson(
            mixed_history,
            nlohmann::json{
                {"material_resources",
                 {
                     {"current",
                      "simulation_color"},
                     {"previous",
                      "simulation_color@history"},
                 }}},
            names, buffers),
        Catch::Matchers::ContainsSubstring(
            "both current and history"));
}

TEST_CASE("material pass contracts parse and filter independently of local pass ids",
          "[renderingpass][material-routing]") {
    REQUIRE(forwardMaterialPassColorAttachmentFormat ==
            vk::Format::eR16G16B16A16Sfloat);
    PassDefinition pass_def;
    pass_def.name = "forward_opaque";
    pass_def.pass_info = MaterialPassInfo{};
    parseMaterialPassInfoFromJson(
        pass_def, nlohmann::json{{"material_contract", "forward_opaque_v1"}});
    REQUIRE(pass_def.materialInfo().contract == MaterialPassContract::forward_opaque_v1);
    REQUIRE(materialPassShaderContract(pass_def.materialInfo().contract) ==
            MaterialShaderContract::forward_scene_color_v1);
    REQUIRE(materialPassAcceptsMaterial(
        pass_def.materialInfo().contract, pass_def.name,
        MaterialRouteClass::forward_opaque,
        MaterialShaderContract::forward_scene_color_v1));
    REQUIRE_FALSE(materialPassAcceptsMaterial(
        pass_def.materialInfo().contract, pass_def.name,
        MaterialRouteClass::forward_transparent,
        MaterialShaderContract::forward_scene_color_v1));
    REQUIRE_FALSE(materialPassAcceptsMaterial(
        pass_def.materialInfo().contract, pass_def.name,
        MaterialRouteClass::forward_opaque,
        MaterialShaderContract::forward_scene_color_v1,
        std::optional<std::string>{"hero_forward"}));
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(
            pass_def, nlohmann::json{{"material_contract", "unknown_v1"}}),
        Catch::Matchers::ContainsSubstring("Unknown material_contract"));

    REQUIRE(materialPassAcceptsMaterial(
        MaterialPassContract::legacy_gbuffer_v1, "geometry",
        MaterialRouteClass::deferred_geometry,
        MaterialShaderContract::legacy_gbuffer_v1));
    REQUIRE(materialPassAcceptsMaterial(
        MaterialPassContract::legacy_gbuffer_v1, "geometry",
        MaterialRouteClass::deferred_geometry,
        MaterialShaderContract::gbuffer_v1));
    REQUIRE_FALSE(materialPassAcceptsMaterial(
        MaterialPassContract::legacy_gbuffer_v1, "geometry",
        MaterialRouteClass::forward_opaque,
        MaterialShaderContract::forward_scene_color_v1));
}

TEST_CASE(
    "material output schemas remove the engine MRT count while preserving "
    "typed attachment validation",
    "[renderingpass][material-output][wp218]") {
    const nlohmann::json declaration{
        {"schema", "pelican.material_outputs"},
        {"version", 1},
        {"name", "project.extended_gbuffer"},
        {"outputs",
         {
             {{"name", "base_color"},
              {"type", "vec4"},
              {"source", "surface.base_color"}},
             {{"name", "normal"},
              {"type", "vec4"},
              {"source", "surface.normal_encoded"}},
             {{"name", "material"},
              {"type", "vec4"},
              {"source", "surface.material"}},
             {{"name", "world_position"},
              {"type", "vec4"},
              {"source", "input.world_position"}},
             {{"name", "emissive"},
              {"type", "vec4"},
              {"source", "surface.emissive"}},
             {{"name", "object_id"},
              {"type", "uint"},
              {"source", "custom"}},
             {{"name", "reactive_mask"},
              {"type", "float"},
              {"source", "custom"}},
         }},
    };

    PassDefinition pass;
    pass.name = "extended_geometry";
    pass.pass_info = MaterialPassInfo{};
    parseMaterialPassInfoFromJson(
        pass,
        nlohmann::json{
            {"material_contract",
             "deferred_geometry_v1"},
            {"material_outputs", declaration},
        });
    REQUIRE(pass.materialInfo().output_schema);
    REQUIRE(
        pass.materialInfo().output_schema->outputs.size() ==
        7);
    REQUIRE(
        materialOutputSchemaFingerprint(
            *pass.materialInfo().output_schema)
            .starts_with(
                "pelican.material_outputs@1:"
                "project.extended_gbuffer"));

    for (int index = 0; index < 7; ++index) {
        pass.output_color.push_back(
            GlobalRenderTargetId{index});
    }
    pass.output_depth = GlobalRenderTargetId{7};
    const RenderTargetMetadataResolver metadata{
        [](GlobalRenderTargetId id) {
            return RenderTargetMetadata{
                .name =
                    id.value == 7
                        ? "depth"
                        : "gbuffer_" +
                              std::to_string(id.value),
                .usage =
                    id.value == 7
                        ? vk::ImageUsageFlagBits::
                              eDepthStencilAttachment
                        : vk::ImageUsageFlagBits::
                              eColorAttachment,
                .format =
                    id.value == 7
                        ? vk::Format::eD16Unorm
                        : id.value == 5
                              ? vk::Format::eR32Uint
                              : vk::Format::
                                    eR16G16B16A16Sfloat,
                .extent = {1280, 720},
            };
        }};
    REQUIRE_NOTHROW(
        validateMaterialPassAttachments(
            pass, metadata));
    REQUIRE(
        pass.colorClearValue(5).numeric_class ==
        MaterialOutputNumericClass::unsigned_integer);
    REQUIRE(
        pass.colorClearValue(5)
            .vulkan()
            .uint32[3] == 1u);

    pass.output_color.pop_back();
    REQUIRE_THROWS_WITH(
        validateMaterialPassAttachments(pass, metadata),
        Catch::Matchers::ContainsSubstring(
            "output.color count must match"));

    auto invalid = declaration;
    invalid["outputs"][0]["type"] = "uint";
    invalid["outputs"][0]["source"] = "custom";
    pass.output_color.push_back(GlobalRenderTargetId{6});
    pass.materialInfo().output_schema =
        parseMaterialOutputSchema(invalid);
    REQUIRE_THROWS_WITH(
        validateMaterialPassAttachments(pass, metadata),
        Catch::Matchers::ContainsSubstring(
            "numeric class is incompatible"));
}

TEST_CASE(
    "material output schema rejects duplicate names and lit data in deferred",
    "[renderingpass][material-output][wp218]") {
    nlohmann::json declaration{
        {"schema", "pelican.material_outputs"},
        {"version", 1},
        {"name", "project.invalid"},
        {"outputs",
         {
             {{"name", "same"},
              {"type", "vec4"},
              {"source", "surface.base_color"}},
             {{"name", "same"},
              {"type", "vec4"},
              {"source", "surface.emissive"}},
         }},
    };
    REQUIRE_THROWS_WITH(
        parseMaterialOutputSchema(declaration),
        Catch::Matchers::ContainsSubstring(
            "output names must be unique"));

    declaration["outputs"].erase(
        declaration["outputs"].begin() + 1);
    declaration["outputs"][0]["name"] = "lit";
    declaration["outputs"][0]["source"] =
        "lighting.scene_color";
    PassDefinition pass;
    pass.name = "deferred";
    pass.pass_info = MaterialPassInfo{};
    pass.materialInfo().contract =
        MaterialPassContract::deferred_geometry_v1;
    pass.materialInfo().output_schema =
        parseMaterialOutputSchema(declaration);
    pass.output_color = {GlobalRenderTargetId{0}};
    pass.output_depth = GlobalRenderTargetId{1};
    const RenderTargetMetadataResolver metadata{
        [](GlobalRenderTargetId id) {
            return RenderTargetMetadata{
                .name =
                    id.value == 0 ? "color" : "depth",
                .usage =
                    id.value == 0
                        ? vk::ImageUsageFlagBits::
                              eColorAttachment
                        : vk::ImageUsageFlagBits::
                              eDepthStencilAttachment,
                .format =
                    id.value == 0
                        ? vk::Format::
                              eR16G16B16A16Sfloat
                        : vk::Format::eD32Sfloat,
                .extent = {1, 1},
            };
        }};
    REQUIRE_THROWS_WITH(
        validateMaterialPassAttachments(pass, metadata),
        Catch::Matchers::ContainsSubstring(
            "cannot use lighting.scene_color"));
}

TEST_CASE(
    "material output attachment state is sparse typed and schema keyed",
    "[renderingpass][material-output-state][wp219]") {
    const auto schema = parseMaterialOutputSchema(
        nlohmann::json{
            {"schema", "pelican.material_outputs"},
            {"version", 1},
            {"name", "project.weighted_oit"},
            {"outputs",
             nlohmann::json::array({
                 {{"name", "accum"},
                  {"type", "vec4"},
                  {"source", "surface.base_color"}},
                 {{"name", "revealage"},
                  {"type", "float"},
                  {"source", "custom"}},
                 {{"name", "object_id"},
                  {"type", "uint"},
                  {"source", "custom"}},
             })},
        });
    const nlohmann::json declaration{
        {"revealage",
         {
             {"blend",
              {
                  {"color",
                   {{"src", "zero"},
                    {"dst", "one_minus_src_color"},
                    {"op", "add"}}},
                  {"alpha",
                   {{"src", "zero"},
                    {"dst", "one_minus_src_alpha"},
                    {"op", "add"}}},
              }},
             {"write_mask", "r"},
         }},
        {"accum",
         {
             {"blend",
              {
                  {"color",
                   {{"src", "one"},
                    {"dst", "one"},
                    {"op", "add"}}},
                  {"alpha",
                   {{"src", "one"},
                    {"dst", "one"},
                    {"op", "add"}}},
              }},
         }},
        {"object_id",
         {
             {"blend", "opaque"},
             {"write_mask", "r"},
         }},
    };
    const auto states =
        parseMaterialOutputAttachmentStates(
            declaration, schema);
    REQUIRE(states.size() == 3);
    REQUIRE(states[0].output == "accum");
    REQUIRE(states[1].output == "revealage");
    REQUIRE(states[2].output == "object_id");
    REQUIRE(states[0].blend);
    REQUIRE(states[0].blend->enabled);
    REQUIRE(
        states[0].blend->color.destination ==
        MaterialOutputBlendFactor::one);
    REQUIRE(states[1].write_mask ==
            materialOutputWriteRed);
    REQUIRE(states[2].blend);
    REQUIRE_FALSE(states[2].blend->enabled);

    const auto canonical =
        materialOutputAttachmentStatesToJson(
            states, schema);
    REQUIRE(
        parseMaterialOutputAttachmentStates(
            canonical, schema) == states);
    REQUIRE(
        materialOutputAttachmentStatesFingerprint(
            states, schema)
            .starts_with(
                "pelican.material_output_states@1:"));

    auto invalid = declaration;
    invalid["object_id"]["blend"] = "additive";
    REQUIRE_THROWS_WITH(
        parseMaterialOutputAttachmentStates(
            invalid, schema),
        Catch::Matchers::ContainsSubstring(
            "cannot enable blending for an integer output"));

    invalid = declaration;
    invalid["accum"]["write_mask"] = "rr";
    REQUIRE_THROWS_WITH(
        parseMaterialOutputAttachmentStates(
            invalid, schema),
        Catch::Matchers::ContainsSubstring(
            "duplicate channel"));

    invalid = declaration;
    invalid["unknown"] = {
        {"write_mask", "rgba"}};
    REQUIRE_THROWS_WITH(
        parseMaterialOutputAttachmentStates(
            invalid, schema),
        Catch::Matchers::ContainsSubstring(
            "references unknown output"));

    PassDefinition pass;
    pass.name = "missing_schema";
    pass.pass_info = MaterialPassInfo{};
    REQUIRE_THROWS_WITH(
        parseMaterialPassInfoFromJson(
            pass,
            nlohmann::json{
                {"material_output_states",
                 nlohmann::json::object()},
            }),
        Catch::Matchers::ContainsSubstring(
            "requires material_outputs"));
}

TEST_CASE("material pass availability distinguishes fullscreen-only graphs",
          "[renderingpass][material-routing]") {
    RenderingPassContainer container;

    PassDefinition fullscreen;
    fullscreen.name = "present";
    fullscreen.pass_info = FullscreenPassInfo{};
    container.registerCompiledRenderingPass(CompiledRenderingPass{
        "fullscreen_only", {{fullscreen, PassId{0}}}, {}});
    REQUIRE_FALSE(container.hasMaterialPasses());

    PassDefinition material;
    material.name = "geometry";
    material.pass_info = MaterialPassInfo{};
    material.rasterization_samples =
        vk::SampleCountFlagBits::e4;
    CompiledPassRenderingContract rendering;
    rendering.scope_index = 2;
    rendering.scope_id = "gbuffer_lighting";
    rendering.color_attachments = {
        GlobalRenderTargetId{4},
        GlobalRenderTargetId{5}};
    rendering.color_attachment_locations = {
        0, unusedPhysicalAttachmentMapping};
    rendering.color_attachment_input_indices = {
        unusedPhysicalAttachmentMapping,
        unusedPhysicalAttachmentMapping};
    rendering.local_read_scope = true;
    container.registerCompiledRenderingPass(CompiledRenderingPass{
        "with_material",
        {{material, PassId{0}, {}, rendering}},
        {}});
    REQUIRE(container.hasMaterialPasses());
    REQUIRE(container.supportsMaterialPass(
        MaterialRouteClass::deferred_geometry,
        MaterialShaderContract::gbuffer_v1));
    const auto bindings =
        container.materialPassRenderingBindings(
            MaterialRouteClass::deferred_geometry,
            MaterialShaderContract::gbuffer_v1);
    REQUIRE(bindings.size() == 1);
    CHECK(bindings.front().pass_name == "geometry");
    CHECK(bindings.front().rasterization_samples ==
          vk::SampleCountFlagBits::e4);
    CHECK(bindings.front().rendering == rendering);
    CHECK(container.materialPassRenderingBindings(
              MaterialRouteClass::forward_opaque,
              MaterialShaderContract::
                  forward_scene_color_v1)
              .empty());
    CHECK(container.materialPassRenderingBindings(
              MaterialRouteClass::deferred_geometry,
              MaterialShaderContract::gbuffer_v1,
              std::optional<std::string>{
                  "another_geometry"})
              .empty());
}

TEST_CASE(
    "material pass container resolves one sampled or local-read shader input ABI across variants",
    "[renderingpass][material][local-read][wp220]") {
    RenderingPassContainer container;
    const GlobalRenderTargetId unrelated{4};
    const GlobalRenderTargetId source{5};
    const GlobalRenderTargetId output{6};

    PassDefinition material;
    material.name = "decal";
    material.pass_info = MaterialPassInfo{};
    material.input_targets = {
        unrelated, source};
    material.input_target_history = {
        false, false};
    material.output_color = {output};
    material.materialInfo()
        .material_resources.push_back(
            MaterialPassResourceBinding{
                .port =
                    ShaderResourcePortDefinition{
                        .name = "gbuffer_normal",
                        .resource = "normal",
                        .kind =
                            ShaderResourcePortKind::
                                image,
                        .access =
                            ShaderResourcePortAccess::
                                sampled,
                    },
                .target = source,
                .footprint = {
                    LogicalReadFootprintKind::
                        same_pixel,
                    std::nullopt,
                },
            });

    CompiledPassRenderingContract rendering;
    rendering.local_read_scope = true;
    rendering.color_attachments = {
        source, output};
    rendering.color_attachment_locations = {
        unusedPhysicalAttachmentMapping, 0};
    rendering.color_attachment_input_indices = {
        1, unusedPhysicalAttachmentMapping};
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "flat",
            {{material, PassId{0}, {}, rendering}},
            {}});

    const std::array requests{
        MaterialPassShaderInputRequest{
            .kind =
                MaterialPassShaderInputKind::
                    material_resource,
            .name = "gbuffer_normal",
        },
    };
    const auto local =
        container.materialPassShaderInputBindings(
            MaterialRouteClass::deferred_geometry,
            MaterialShaderContract::gbuffer_v1,
            requests);
    REQUIRE(local.size() == 1);
    REQUIRE(
        local.front().input_attachment_index ==
        1u);

    auto sampled_rendering = rendering;
    sampled_rendering.local_read_scope = false;
    sampled_rendering
        .color_attachment_input_indices = {
            unusedPhysicalAttachmentMapping,
            unusedPhysicalAttachmentMapping};
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "desktop",
            {{material, PassId{0}, {},
              sampled_rendering}},
            {}});
    REQUIRE_THROWS_WITH(
        container.materialPassShaderInputBindings(
            MaterialRouteClass::deferred_geometry,
            MaterialShaderContract::gbuffer_v1,
            requests),
        Catch::Matchers::ContainsSubstring(
            "different sampled/local-read ABIs"));
}

TEST_CASE(
    "material pass container resolves one output ABI across runtime variants",
    "[renderingpass][material-output][wp218]") {
    RenderingPassContainer container;
    const MaterialOutputSchema schema{
        .name = "project.runtime_gbuffer",
        .outputs = {
            {"base_color", MaterialOutputType::vec4,
             MaterialOutputSource::surface_base_color},
            {"object_id",
             MaterialOutputType::unsigned_integer,
             MaterialOutputSource::custom},
        },
    };
    PassDefinition flat;
    flat.name = "deferred_geometry";
    flat.pass_info = MaterialPassInfo{
        .contract =
            MaterialPassContract::
                deferred_geometry_v1,
        .output_schema = schema,
    };
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "flat", {{flat, PassId{0}}}, {}});
    REQUIRE(
        container.materialOutputSchema(
            MaterialRouteClass::deferred_geometry) ==
        schema);
    REQUIRE_FALSE(
        container.materialOutputSchema(
            MaterialRouteClass::forward_opaque));

    auto xr = flat;
    xr.materialInfo().output_schema->name =
        "project.xr_drift";
    container.registerCompiledRenderingPass(
        CompiledRenderingPass{
            "xr", {{xr, PassId{0}}}, {}});
    REQUIRE_THROWS_WITH(
        container.materialOutputSchema(
            MaterialRouteClass::deferred_geometry),
        Catch::Matchers::ContainsSubstring(
            "different material_outputs schemas"));
}

TEST_CASE("material pass info parser preserves defaults when material range is omitted", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";
    pass_def.pass_info = MaterialPassInfo{};

    parseMaterialPassInfoFromJson(pass_def, nlohmann::json::object());

    REQUIRE(pass_def.materialInfo().material_start == 0);
    REQUIRE(pass_def.materialInfo().material_count == 0);
}

TEST_CASE("material pass info parser rejects malformed material range", "[renderingpass]") {
    PassDefinition pass_def;
    pass_def.name = "geometry";
    pass_def.pass_info = MaterialPassInfo{};

    const nlohmann::json pass_json{
        {"material_range", 1},
    };

    REQUIRE_THROWS_AS(parseMaterialPassInfoFromJson(pass_def, pass_json), std::runtime_error);
}

TEST_CASE("rendering pass JSON helpers reject malformed values", "[renderingpass]") {
    REQUIRE_THROWS_AS(stringToFormat("UNKNOWN"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToUsageFlags({}), std::runtime_error);
    REQUIRE_THROWS_AS(makePassInfo("compute"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToFullscreenPushConstantData("camera"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToLoadOp("keep"), std::runtime_error);
    REQUIRE_THROWS_AS(stringToStoreOp("load"), std::runtime_error);

    REQUIRE_THROWS_AS(validateName("", "Pass"), std::runtime_error);
    REQUIRE_THROWS_AS(parseStringField(nlohmann::json{{"name", 1}}, "name", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(parseFloatField(nlohmann::json{{"scale", "large"}}, "scale", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(parseStringArrayField(nlohmann::json{{"usage", nlohmann::json::array({"SAMPLED", 1})}},
                                            "usage", "test"),
                      std::runtime_error);
    REQUIRE_THROWS_AS(parseUint32Field(nlohmann::json{{"count", -1}}, "count", "test"), std::runtime_error);
    REQUIRE_THROWS_AS(jsonToClearColor(nlohmann::json::array({1.0f, 0.0f, 0.0f})), std::runtime_error);
    REQUIRE_THROWS_AS(parseRenderTargetDefinitionsFromJson(nlohmann::json{{"render_targets", 1}}),
                      std::runtime_error);
}

TEST_CASE("rendering pass runtime compiler pairs definitions with pass ids", "[renderingpass]") {
    PassDefinition material_pass;
    material_pass.name = "geometry";
    material_pass.pass_info = MaterialPassInfo{};

    PassDefinition ui_pass;
    ui_pass.name = "ui";
    ui_pass.pass_info = UiPassInfo{};

    RenderingPassDefinition definition;
    definition.name = "main";
    definition.passes = {material_pass, ui_pass};

    const auto compiled = compileRenderingPassRuntime(definition);

    REQUIRE(compiled.name == "main");
    REQUIRE(compiled.passes.size() == 2);
    REQUIRE(compiled.passes[0].definition.name == "geometry");
    REQUIRE(compiled.passes[0].pass_id.value == 0);
    REQUIRE(compiled.passes[1].definition.name == "ui");
    REQUIRE(compiled.passes[1].pass_id.value == 1);
    REQUIRE(compiled.passes[1].definition.isUi());
}

TEST_CASE("rendering pass runtime compiler compiles definition lists", "[renderingpass]") {
    PassDefinition material_pass;
    material_pass.name = "geometry";
    material_pass.pass_info = MaterialPassInfo{};

    RenderingPassDefinition main_definition;
    main_definition.name = "main";
    main_definition.passes = {material_pass};

    RenderingPassDefinition shadow_definition;
    shadow_definition.name = "shadow";
    shadow_definition.passes = {material_pass};

    const auto compiled = compileRenderingPassesRuntime({main_definition, shadow_definition});

    REQUIRE(compiled.size() == 2);
    REQUIRE(compiled[0].name == "main");
    REQUIRE(compiled[1].name == "shadow");
    REQUIRE(compiled[1].passes[0].pass_id.value == 0);
}

TEST_CASE("rendering pass runtime compiler requires fullscreen dependencies", "[renderingpass]") {
    PassDefinition fullscreen_pass;
    fullscreen_pass.name = "postprocess";
    fullscreen_pass.pass_info = FullscreenPassInfo{};
    fullscreen_pass.output_color = {swapchainRenderTargetId()};

    RenderingPassDefinition definition;
    definition.name = "main";
    definition.passes = {fullscreen_pass};

    REQUIRE_THROWS_AS(compileRenderingPassRuntime(definition), std::runtime_error);
}

TEST_CASE(
    "rendering pass runtime compiler applies per-attachment physical operations",
    "[renderingpass][physical-attachment]") {
    PassDefinition pass;
    pass.name = "geometry";
    pass.pass_info = MaterialPassInfo{};
    pass.output_color = {
        swapchainRenderTargetId()};

    RenderingPassDefinition definition;
    definition.name = "main";
    definition.passes = {pass};

    VulkanTargetPlan plan;
    plan.scopes = {
        {
            .id = "geometry",
            .nodes = {"geometry"},
        },
    };
    plan.attachments = {
        {
            .node = "geometry",
            .logical_resource = "swapchain",
            .aspect =
                VulkanPhysicalAttachmentAspect::color,
            .load_op =
                VulkanPhysicalAttachmentLoadOp::discard,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::discard,
        },
    };

    const auto compiled =
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .target_plan = &plan,
            });
    const auto &physical =
        compiled.passes.front().definition;
    REQUIRE(
        physical.color_load_op ==
        vk::AttachmentLoadOp::eClear);
    REQUIRE((
        physical.colorAttachmentOperations(0) ==
        PassAttachmentOperations{
            vk::AttachmentLoadOp::eDontCare,
            vk::AttachmentStoreOp::eDontCare,
        }));

    plan.attachments.front().aspect =
        VulkanPhysicalAttachmentAspect::depth;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .target_plan = &plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "attachment aspect disagrees"));
}

TEST_CASE(
    "runtime target format selects typed clear values independently of material schema",
    "[renderingpass][material-output][typed-clear][wp218]") {
    const GlobalRenderTargetId object_id{0};
    const RenderTargetMetadataResolver metadata{
        [=](GlobalRenderTargetId id) {
            REQUIRE(id == object_id);
            return RenderTargetMetadata{
                .name = "object_id",
                .usage =
                    vk::ImageUsageFlagBits::
                        eColorAttachment,
                .format = vk::Format::eR32Uint,
                .extent = {64, 64},
            };
        }};
    PassDefinition pass;
    pass.name = "clear_object_id";
    pass.pass_info = UiPassInfo{};
    pass.output_color = {object_id};
    pass.clear_color = {
        4294967295.0, 7.0, 0.0, 1.0};

    const auto compiled =
        compileRenderingPassRuntime(
            RenderingPassDefinition{
                .name = "typed_clear",
                .passes = {pass},
            },
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
            });
    const auto &physical =
        compiled.passes.front().definition;
    REQUIRE(
        physical
            .physical_color_numeric_classes ==
        std::vector<MaterialOutputNumericClass>{
            MaterialOutputNumericClass::
                unsigned_integer});
    const auto clear =
        physical.colorClearValue(0);
    REQUIRE(
        clear.numeric_class ==
        MaterialOutputNumericClass::
            unsigned_integer);
    REQUIRE(
        clear.vulkan().uint32[0] ==
        std::numeric_limits<
            std::uint32_t>::max());
    REQUIRE(clear.vulkan().uint32[1] == 7u);

    PassDefinition floating;
    floating.output_color = {object_id};
    floating.clear_color = {
        std::numeric_limits<double>::max(),
        0.0, 0.0, 0.0};
    REQUIRE_THROWS_WITH(
        floating.colorClearValue(0),
        "floating color clear value is out of range");
}

TEST_CASE(
    "rendering pass runtime compiler expands fused attachment mappings",
    "[renderingpass][physical-scope][local-read]") {
    const GlobalRenderTargetId gbuffer{0};
    const GlobalRenderTargetId lit{1};
    const RenderTargetMetadataResolver metadata{
        [=](GlobalRenderTargetId id) {
            if (id == gbuffer) {
                return RenderTargetMetadata{
                    .name = "gbuffer",
                    .usage =
                        vk::ImageUsageFlagBits::
                            eColorAttachment,
                    .format =
                        vk::Format::eR8G8B8A8Unorm,
                    .extent = {64, 64},
                };
            }
            if (id == lit) {
                return RenderTargetMetadata{
                    .name = "lit",
                    .usage =
                        vk::ImageUsageFlagBits::
                            eColorAttachment,
                    .format =
                        vk::Format::
                            eR16G16B16A16Sfloat,
                    .extent = {64, 64},
                };
            }
            throw std::runtime_error(
                "unknown test render target");
        }};

    PassDefinition geometry;
    geometry.name = "geometry";
    geometry.pass_info = MaterialPassInfo{};
    geometry.output_color = {gbuffer};
    geometry.clear_color =
        {0.25, 0.5, 0.75, 1.0};

    PassDefinition lighting;
    lighting.name = "lighting";
    lighting.pass_info = MaterialPassInfo{};
    lighting.input_targets = {gbuffer};
    lighting.input_target_history = {false};
    lighting.output_color = {lit};
    lighting.clear_color =
        {0.0, 0.0, 0.0, 1.0};

    const RenderingPassDefinition definition{
        .name = "main",
        .passes = {geometry, lighting},
    };
    VulkanTargetPlan plan;
    plan.resources = {
        {
            .logical_resource = "gbuffer",
            .representation =
                VulkanResourceRepresentation::
                    tile_local_attachment,
        },
        {
            .logical_resource = "lit",
            .representation =
                VulkanResourceRepresentation::
                    materialized_image,
        },
    };
    plan.scopes = {
        {
            .id = "geometry_lighting",
            .nodes = {"geometry", "lighting"},
            .single_rendering_instance = true,
            .local_reads = {"gbuffer"},
        },
    };
    plan.attachments = {
        {
            .node = "geometry",
            .logical_resource = "gbuffer",
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    discard,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    discard,
        },
        {
            .node = "lighting",
            .logical_resource = "lit",
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    clear,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    store,
        },
    };

    const auto compiled =
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &plan,
            });
    REQUIRE(compiled.passes.size() == 2);
    const auto &geometry_contract =
        compiled.passes.at(0).rendering;
    const auto &lighting_contract =
        compiled.passes.at(1).rendering;
    REQUIRE(geometry_contract.scope_index == 0);
    REQUIRE(
        geometry_contract.scope_id ==
        "geometry_lighting");
    REQUIRE(
        geometry_contract.color_attachments ==
        std::vector<GlobalRenderTargetId>{
            gbuffer, lit});
    REQUIRE(
        geometry_contract
            .color_attachment_locations ==
        std::vector<std::uint32_t>{
            0,
            unusedPhysicalAttachmentMapping});
    REQUIRE(
        geometry_contract
            .color_attachment_input_indices ==
        std::vector<std::uint32_t>{
            unusedPhysicalAttachmentMapping,
            unusedPhysicalAttachmentMapping});
    REQUIRE(
        lighting_contract
            .color_attachment_locations ==
        std::vector<std::uint32_t>{
            unusedPhysicalAttachmentMapping,
            0});
    REQUIRE(
        lighting_contract
            .color_attachment_input_indices ==
        std::vector<std::uint32_t>{
            0,
            unusedPhysicalAttachmentMapping});
    REQUIRE(
        lighting_contract.local_read_scope);
    const auto expected_scope_operations =
        std::vector<PassAttachmentOperations>{
            {
                vk::AttachmentLoadOp::eDontCare,
                vk::AttachmentStoreOp::eDontCare,
            },
            {
                vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore,
            },
        };
    REQUIRE(
        geometry_contract
            .scope_color_attachment_operations ==
        expected_scope_operations);
    REQUIRE(
        lighting_contract
            .scope_color_attachment_operations ==
        expected_scope_operations);
    const auto expected_scope_clear_values =
        std::vector<PhysicalColorClearValue>{
            {
                .numeric_class =
                    MaterialOutputNumericClass::floating,
                .floating =
                    {0.25f, 0.5f, 0.75f, 1.0f},
                .signed_integer = {},
                .unsigned_integer = {},
            },
            {
                .numeric_class =
                    MaterialOutputNumericClass::floating,
                .floating =
                    {0.0f, 0.0f, 0.0f, 1.0f},
                .signed_integer = {},
                .unsigned_integer = {},
            },
        };
    REQUIRE(
        geometry_contract.scope_color_clear_values ==
        expected_scope_clear_values);
    REQUIRE(
        lighting_contract.scope_color_clear_values ==
        geometry_contract.scope_color_clear_values);

    auto repeated_definition = definition;
    repeated_definition.passes.front()
        .output_color.push_back(lit);
    auto repeated_plan = plan;
    repeated_plan.attachments.insert(
        repeated_plan.attachments.begin() + 1,
        VulkanPhysicalAttachmentPlan{
            .node = "geometry",
            .logical_resource = "lit",
            .load_op =
                VulkanPhysicalAttachmentLoadOp::clear,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::store,
        });
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            repeated_definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &repeated_plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "cannot preserve intermediate materialized color "
            "attachment operations"));
    repeated_plan.attachments.back().load_op =
        VulkanPhysicalAttachmentLoadOp::load;
    REQUIRE_NOTHROW(
        compileRenderingPassRuntime(
            repeated_definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &repeated_plan,
            }));

    plan.scopes.front().local_reads.clear();
    plan.scopes.front()
        .single_rendering_instance = false;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata = &metadata,
                .target_plan = &plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "tile-local pass input is not declared"));
}

TEST_CASE(
    "rendering pass runtime compiler materializes one dynamic-rendering instance for fused scopes",
    "[renderingpass][physical-scope][materialized][fusion]") {
    const GlobalRenderTargetId color{0};
    const RenderTargetMetadataResolver metadata{
        [=](GlobalRenderTargetId id) {
            if (id != color) {
                throw std::runtime_error(
                    "unknown test render target");
            }
            return RenderTargetMetadata{
                .name = "scene_color",
                .usage =
                    vk::ImageUsageFlagBits::
                        eColorAttachment,
                .format =
                    vk::Format::eR8G8B8A8Unorm,
                .extent = {64, 64},
            };
        }};

    PassDefinition base;
    base.name = "base";
    base.pass_info = MaterialPassInfo{};
    base.output_color = {color};

    PassDefinition overlay;
    overlay.name = "overlay";
    overlay.pass_info = MaterialPassInfo{};
    overlay.output_color = {color};
    overlay.color_load_op =
        vk::AttachmentLoadOp::eLoad;

    const RenderingPassDefinition definition{
        .name = "main",
        .passes = {base, overlay},
    };
    VulkanTargetPlan plan;
    plan.scopes = {
        {
            .id = "base_overlay",
            .nodes = {"base", "overlay"},
            .single_rendering_instance = true,
        },
    };
    plan.attachments = {
        {
            .node = "base",
            .logical_resource =
                "scene_color",
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    clear,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    store,
        },
        {
            .node = "overlay",
            .logical_resource =
                "scene_color",
            .load_op =
                VulkanPhysicalAttachmentLoadOp::
                    load,
            .store_op =
                VulkanPhysicalAttachmentStoreOp::
                    store,
        },
    };

    const auto compiled =
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
                .target_plan = &plan,
            });
    REQUIRE(compiled.passes.size() == 2);
    REQUIRE(
        compiled.passes[0].rendering
            .fused_rendering_scope);
    REQUIRE(
        compiled.passes[1].rendering
            .fused_rendering_scope);
    REQUIRE_FALSE(
        compiled.passes[0].rendering
            .local_read_scope);
    REQUIRE(
        compiled.passes[0].rendering
            .scope_color_attachment_operations ==
        std::vector<PassAttachmentOperations>{
            {
                vk::AttachmentLoadOp::eClear,
                vk::AttachmentStoreOp::eStore,
            }});

    auto multiview_plan = plan;
    multiview_plan.view_execution_plan
        .view_count = 2;
    multiview_plan.view_execution_plan
        .uses_multiview = true;
    multiview_plan.scopes.front()
        .view_execution =
        VulkanScopeViewExecution::multiview;
    multiview_plan.scopes.front()
        .view_count = 2;
    multiview_plan.scopes.front()
        .execution_count = 1;
    multiview_plan.scopes.front()
        .view_mask = 0b11;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
                .target_plan =
                    &multiview_plan,
            }),
        Catch::Matchers::ContainsSubstring(
            "unsupported production pass implementation: base"));

    auto invalid = plan;
    invalid.attachments[1].load_op =
        VulkanPhysicalAttachmentLoadOp::
            clear;
    REQUIRE_THROWS_WITH(
        compileRenderingPassRuntime(
            definition,
            RenderingPassRuntimeDependencies{
                .render_target_metadata =
                    &metadata,
                .target_plan = &invalid,
            }),
        Catch::Matchers::ContainsSubstring(
            "must Load every attachment"));
}

TEST_CASE(
    "view-family scheduler expands mixed physical scopes in scope order",
    "[renderingpass][view-execution][schedule]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "SharedShadow"},
        {.name = "GBuffer"},
        {.name = "Transparent"},
        {.name = "Present"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.view_execution_plan.uses_multiview = true;
    plan.view_execution_plan.mixed_execution = true;
    plan.scopes = {
        {
            .id = "shared",
            .nodes = {"SharedShadow"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
        },
        {
            .id = "gbuffer",
            .nodes = {"GBuffer"},
            .view_execution =
                VulkanScopeViewExecution::multiview,
            .view_count = 2,
            .execution_count = 1,
            .view_mask = 0b11,
        },
        {
            .id = "sequential",
            .nodes = {"Transparent"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 2,
        },
        {
            .id = "present",
            .nodes = {"Present"},
            .view_execution =
                VulkanScopeViewExecution::multiview,
            .view_count = 2,
            .execution_count = 1,
            .view_mask = 0b11,
        },
    };

    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 2);
    REQUIRE(schedule.size() == 5);
    REQUIRE(schedule[0].node_index == 0);
    REQUIRE(schedule[0].execution ==
            VulkanScopeViewExecution::single_view);
    REQUIRE(schedule[1].node_index == 1);
    REQUIRE(schedule[1].execution ==
            VulkanScopeViewExecution::multiview);
    REQUIRE(schedule[2].node_index == 2);
    REQUIRE(schedule[2].view_index == 0);
    REQUIRE(schedule[2].firstExecution());
    REQUIRE_FALSE(schedule[2].lastExecution());
    REQUIRE(schedule[3].node_index == 2);
    REQUIRE(schedule[3].view_index == 1);
    REQUIRE_FALSE(schedule[3].firstExecution());
    REQUIRE(schedule[3].lastExecution());
    REQUIRE(schedule[4].node_index == 3);
    REQUIRE(schedule[4].execution ==
            VulkanScopeViewExecution::multiview);
    REQUIRE(schedule[4].logical_view_count == 2);
}

TEST_CASE(
    "view-family scheduler keeps fused sequential scopes together per view",
    "[renderingpass][view-execution][schedule][scope]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "Geometry"},
        {.name = "Lighting"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.scopes = {
        {
            .id = "fused",
            .nodes = {"Geometry", "Lighting"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 2,
        },
    };

    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 2);
    REQUIRE(schedule.size() == 4);
    REQUIRE(schedule[0].node_index == 0);
    REQUIRE(schedule[0].view_index == 0);
    REQUIRE(schedule[0].beginsScopeExecution());
    REQUIRE_FALSE(
        schedule[0].endsScopeExecution());
    REQUIRE(schedule[1].node_index == 1);
    REQUIRE(schedule[1].view_index == 0);
    REQUIRE_FALSE(
        schedule[1].beginsScopeExecution());
    REQUIRE(schedule[1].endsScopeExecution());
    REQUIRE(schedule[2].node_index == 0);
    REQUIRE(schedule[2].view_index == 1);
    REQUIRE(schedule[2].beginsScopeExecution());
    REQUIRE(schedule[3].node_index == 1);
    REQUIRE(schedule[3].view_index == 1);
    REQUIRE(schedule[3].endsScopeExecution());
}

TEST_CASE(
    "view-family scheduler follows dependency-verified physical order",
    "[renderingpass][view-execution][schedule][reorder]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "A"},
        {.name = "B"},
        {.name = "C"},
        {.name = "D"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 1;
    plan.scopes = {
        {
            .id = "reordered",
            .nodes = {"C"},
        },
        {
            .id = "non_contiguous_fused",
            .nodes = {"A", "D"},
            .single_rendering_instance = true,
        },
        {
            .id = "tail",
            .nodes = {"B"},
        },
    };

    const auto schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 1);
    REQUIRE(schedule.size() == 4);
    REQUIRE(
        std::vector<std::size_t>{
            schedule[0].node_index,
            schedule[1].node_index,
            schedule[2].node_index,
            schedule[3].node_index} ==
        std::vector<std::size_t>{2, 0, 3, 1});
    REQUIRE(
        schedule[1].beginsScopeExecution());
    REQUIRE_FALSE(
        schedule[1].endsScopeExecution());
    REQUIRE(
        schedule[2].endsScopeExecution());
    REQUIRE(
        schedule[1].scope_index ==
        schedule[2].scope_index);
}

TEST_CASE(
    "per-view scheduler preserves complete physical scopes",
    "[renderingpass][view-execution][schedule][scope]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "Shared"},
        {.name = "Geometry"},
        {.name = "Lighting"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.scopes = {
        {
            .id = "shared",
            .nodes = {"Shared"},
            .view_execution =
                VulkanScopeViewExecution::single_view,
            .view_count = 1,
            .execution_count = 1,
        },
        {
            .id = "fused",
            .nodes = {"Geometry", "Lighting"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 2,
        },
    };

    const auto logical_frame_schedule =
        buildLogicalFrameViewFamilySchedule(
            nodes, plan, 2);
    const auto first_view =
        selectLogicalFrameSequentialViewSchedule(
            logical_frame_schedule, 0, 2);
    const auto second_view =
        selectLogicalFrameSequentialViewSchedule(
            logical_frame_schedule, 1, 2);

    REQUIRE(first_view.size() == 3);
    REQUIRE(first_view[0].node_index == 0);
    REQUIRE(first_view[1].node_index == 1);
    REQUIRE(first_view[1].beginsScopeExecution());
    REQUIRE(first_view[2].node_index == 2);
    REQUIRE(first_view[2].endsScopeExecution());
    REQUIRE(second_view.size() == 2);
    REQUIRE(second_view[0].node_index == 1);
    REQUIRE(second_view[0].beginsScopeExecution());
    REQUIRE(second_view[1].node_index == 2);
    REQUIRE(second_view[1].endsScopeExecution());
}

TEST_CASE(
    "view-family scheduler rejects incomplete or inconsistent physical contracts",
    "[renderingpass][view-execution][schedule][diagnostic]") {
    const std::vector<FrameGraphExecutionNode> nodes{
        {.name = "A"},
        {.name = "B"},
    };
    VulkanTargetPlan plan;
    plan.view_execution_plan.view_count = 2;
    plan.scopes = {
        {
            .id = "broken",
            .nodes = {"A"},
            .view_execution =
                VulkanScopeViewExecution::sequential,
            .view_count = 2,
            .execution_count = 1,
        },
    };
    const auto require_error =
        [&](std::uint32_t view_count,
            std::string_view expected) {
            try {
                (void)buildLogicalFrameViewFamilySchedule(
                    nodes, plan, view_count);
                FAIL("schedule did not reject an invalid contract");
            } catch (const std::runtime_error &error) {
                REQUIRE(std::string_view{error.what()}.find(expected) !=
                        std::string_view::npos);
            }
        };
    require_error(
        2, "inconsistent view-execution contract");

    plan.scopes.front().execution_count = 2;
    require_error(2, "no scope");
    require_error(3, "view count");
}

TEST_CASE(
    "view-layer copy planning preserves sequential and family image ranges",
    "[renderingpass][view-execution][layer-copy]") {
    REQUIRE(
        planViewLayerCopy(
            1, 1, 1, 1, 2, false) ==
        ViewLayerCopyPlan{
            .source_base_array_layer = 0,
            .destination_base_array_layer = 1,
            .array_layers = 1,
        });
    REQUIRE(
        planViewLayerCopy(
            2, 1, 1, 1, 2, false) ==
        ViewLayerCopyPlan{
            .source_base_array_layer = 1,
            .destination_base_array_layer = 1,
            .array_layers = 1,
        });
    REQUIRE(
        planViewLayerCopy(
            2, 0, 2, 0, 2, true) ==
        ViewLayerCopyPlan{
            .source_base_array_layer = 0,
            .destination_base_array_layer = 0,
            .array_layers = 2,
        });
    REQUIRE_THROWS_WITH(
        planViewLayerCopy(
            1, 0, 2, 0, 2, true),
        Catch::Matchers::ContainsSubstring(
            "does not preserve every logical view layer"));
}

} // namespace Pelican
