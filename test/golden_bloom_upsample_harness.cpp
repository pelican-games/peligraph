#include "bloom_upsample_oracle.hpp"
#include "golden_harness.hpp"
#include "vulkan_test_support.hpp"

#include "../src/core/appflow/enginetime.hpp"
#include "../src/core/container.hpp"
#include "../src/core/fullscreenpass/fullscreenpasscontainer.hpp"
#include "../src/core/launchconfig.hpp"
#include "../src/core/log.hpp"
#include "../src/core/loader/pathresolver.hpp"
#include "../src/core/loader/projectsrc.hpp"
#include "../src/core/renderingpass/framegraphruntime.hpp"
#include "../src/core/renderingpass/renderingpasscontainer.hpp"
#include "../src/core/renderingpass/rendertargetcontainer.hpp"
#include "../src/core/shader/pipelinefactory.hpp"
#include "../src/core/vkcore/core.hpp"
#include "../src/core/vkcore/renderer.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Pelican {
namespace {

constexpr vk::Extent2D wp357RenderExtent{160, 90};
constexpr std::array<vk::Extent2D, 4> wp357LevelExtents{
    vk::Extent2D{80, 45},
    vk::Extent2D{40, 22},
    vk::Extent2D{20, 11},
    vk::Extent2D{10, 5},
};
constexpr std::array<std::string_view, 4> wp357LevelTargets{
    "Bloom_Upsample_V_0_RT",
    "Bloom_Upsample_V_1_RT",
    "Bloom_Upsample_V_2_RT",
    "Bloom_Upsample_V_3_RT",
};
constexpr std::array<std::string_view, 3> wp357PassNames{
    "UpsampleBlend_3", "UpsampleBlend_2", "UpsampleBlend_1"};
constexpr std::array<std::size_t, 3> wp357DestinationLevels{2, 1, 0};

std::filesystem::path wp357SourceRoot() {
    return std::filesystem::path{PELICAN_TEST_SOURCE_DIR};
}

std::filesystem::path wp357TempProject(std::string_view name) {
    static std::uint64_t serial = 0;
    const auto nonce =
        std::chrono::steady_clock::now().time_since_epoch().count();
    auto result = std::filesystem::temp_directory_path() /
        ("pelican_wp357_" + std::string{name} + "_" +
         std::to_string(nonce) + "_" + std::to_string(++serial));
    std::filesystem::create_directories(result);
    return result;
}

void wp357WriteText(const std::filesystem::path &path,
                    const std::string &contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        throw std::runtime_error("failed to write WP357 fixture: " +
                                 path.string());
    }
    output << contents;
}

void wp357InstallPrecompiledFragment(const std::filesystem::path &root,
                                     std::string_view stem) {
    const auto source =
        std::filesystem::path{PELICAN_WP357_PRECOMPILED_SHADER_DIR} /
        (std::string{stem} + ".frag.spv");
    const auto destination =
        root / "shaders" / (std::string{stem} + ".frag.spv");
    std::filesystem::create_directories(destination.parent_path());
    std::error_code error;
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing,
        error);
    if (error) {
        throw std::runtime_error(
            "failed to install WP357 precompiled fixture shader " +
            source.string() + ": " + error.message());
    }
}

nlohmann::json wp357ReadJson(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error("failed to read WP357 fixture: " +
                                 path.string());
    }
    return nlohmann::json::parse(input);
}

nlohmann::json wp357ProjectJson(std::string_view name,
                                std::string_view graph) {
    return {
        {"schema", "pelican.project"},
        {"version", 1},
        {"name", std::string{name}},
        {"engine_min_version", "0.1.0"},
        {"basic_config",
         {{"window_title", std::string{name}},
          {"window_size",
           {{"width", wp357RenderExtent.width},
            {"height", wp357RenderExtent.height}}},
          {"fullscreen", false},
          {"framerate", 60},
          {"default_scene_id", "default_scene"},
          {"scene_data_json", "scene.json"},
          {"asset_data_json", "assets.json"},
          {"rendering_config_json", "passes/main.json"},
          {"default_rendering_pass", std::string{graph}}}},
    };
}

void wp357WriteProjectShell(const std::filesystem::path &root,
                            std::string_view name,
                            std::string_view graph) {
    wp357WriteText(root / "project.json",
                   wp357ProjectJson(name, graph).dump(2));
    wp357WriteText(
        root / "scene.json",
        R"json({"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[]}}})json");
    wp357WriteText(
        root / "assets.json",
        R"json({"schema":"pelican.asset_data","version":1,"models":[]})json");
}

nlohmann::json wp357ShippingPass(const nlohmann::json &shipping,
                                 std::string_view name) {
    for (const auto &rendering_pass : shipping.at("rendering_passes")) {
        for (const auto &pass : rendering_pass.at("passes")) {
            if (pass.value("name", std::string{}) == name) return pass;
        }
    }
    throw std::runtime_error("WP357 shipping pass not found: " +
                             std::string{name});
}

nlohmann::json wp357ShippingTarget(const nlohmann::json &shipping,
                                   std::string_view name) {
    for (const auto &target : shipping.at("render_targets")) {
        if (target.value("name", std::string{}) == name) return target;
    }
    throw std::runtime_error("WP357 shipping target not found: " +
                             std::string{name});
}

std::string wp357ProducerShader(std::size_t level) {
    const auto extent = wp357LevelExtents.at(level);
    std::ostringstream source;
    source << R"glsl(#version 460
layout(location = 0) out vec4 outColor;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    float x = float(p.x) / float()glsl"
           << extent.width - 1u << R"glsl();
    float y = float(p.y) / float()glsl"
           << extent.height - 1u << R"glsl();
    float edge = (p.x == 0 ? 0.011 : 0.0) +
                 (p.x == )glsl"
           << extent.width - 1u << R"glsl( ? 0.019 : 0.0) +
                 (p.y == 0 ? 0.007 : 0.0) +
                 (p.y == )glsl"
           << extent.height - 1u << R"glsl( ? 0.013 : 0.0);
    float stage = float()glsl"
           << level << R"glsl();
    vec4 value = vec4(
        0.018 + 0.004 * stage + 0.027 * x + 0.016 * y + edge,
        0.021 + 0.003 * stage + 0.014 * x + 0.031 * y + 0.5 * edge,
        0.015 + 0.005 * stage + 0.023 * x + 0.021 * y + 0.75 * edge,
        0.024 + 0.004 * stage + 0.019 * x + 0.017 * y + 0.6 * edge);
    outColor = value;
}
)glsl";
    return source.str();
}

nlohmann::json wp357BloomConfig(bool hdr,
                                std::optional<std::size_t> peeled_stage) {
    const auto shipping = wp357ReadJson(
        wp357SourceRoot() / "projects" / "example" / "passes" /
        "main_rendering_config.json");
    const std::string format = hdr ? "R16G16B16A16_SFLOAT"
                                   : "R8G8B8A8_SRGB";
    nlohmann::json targets = nlohmann::json::array();
    for (std::size_t level = 0; level < 4; ++level) {
        auto working = wp357ShippingTarget(shipping, wp357LevelTargets[level]);
        working["format"] = format;
        working["format_class"] = "explicit(" + format + ")";
        working["role"] = "color";
        working["usage"] = nlohmann::json::array(
            {"COLOR_ATTACHMENT", "SAMPLED"});
        working["usage"].push_back("TRANSFER_SRC");
        targets.push_back(std::move(working));

        auto reference = wp357ShippingTarget(shipping, wp357LevelTargets[level]);
        reference["name"] = "wp357_seed_V" + std::to_string(level);
        reference["format"] = format;
        reference["format_class"] = "explicit(" + format + ")";
        reference["role"] = "color";
        reference["usage"] =
            nlohmann::json::array({"COLOR_ATTACHMENT", "TRANSFER_SRC"});
        targets.push_back(std::move(reference));
    }

    nlohmann::json passes = nlohmann::json::array();
    for (std::size_t level = 0; level < 4; ++level) {
        const auto producer = [&](std::string name,
                                  std::string target) {
            return nlohmann::json{
                {"name", std::move(name)},
                {"type", "fullscreen"},
                {"output",
                 {{"color", nlohmann::json::array({std::move(target)})},
                  {"depth", nullptr}}},
                {"input", nlohmann::json::array()},
                {"shader",
                 {{"vertex", "engine://fullscreen"},
                  {"fragment",
                   "project://shaders/wp357_seed_V" +
                       std::to_string(level)}}},
                {"clear_color",
                 nlohmann::json::array({0.0, 0.0, 0.0, 0.0})},
            };
        };
        passes.push_back(producer(
            "wp357_working_V" + std::to_string(level),
            std::string{wp357LevelTargets[level]}));
        passes.push_back(producer(
            "wp357_reference_V" + std::to_string(level),
            "wp357_seed_V" + std::to_string(level)));
    }
    for (std::size_t stage = 0; stage < wp357PassNames.size(); ++stage) {
        auto pass = wp357ShippingPass(shipping, wp357PassNames[stage]);
        pass["input_sampling"] = nlohmann::json::array(
            {{{"filter", "linear"}, {"address", "repeat"}}});
        if (peeled_stage == stage) pass.erase("raster_state");
        passes.push_back(std::move(pass));
    }
    passes.push_back({
        {"name", "wp357_present"},
        {"type", "fullscreen"},
        {"after", nlohmann::json::array({"UpsampleBlend_1"})},
        {"output",
         {{"color", nlohmann::json::array({"swapchain"})},
          {"depth", nullptr}}},
        {"input", nlohmann::json::array({wp357LevelTargets[0]})},
        {"shader",
         {{"vertex", "engine://fullscreen"},
          {"fragment", "engine://scene_present"}}},
    });

    return {
        {"features", nlohmann::json::array()},
        {"render_targets", std::move(targets)},
        {"rendering_passes",
         nlohmann::json::array({
             {{"name", "main_render"}, {"passes", std::move(passes)}}})},
    };
}

void wp357WriteBloomProject(const std::filesystem::path &root,
                            bool hdr,
                            std::optional<std::size_t> peeled_stage) {
    wp357WriteProjectShell(root, "WP357 bloom oracle", "main_render");
    for (std::size_t level = 0; level < 4; ++level) {
        wp357WriteText(root / "shaders" /
                           ("wp357_seed_V" + std::to_string(level) + ".frag"),
                       wp357ProducerShader(level));
        wp357InstallPrecompiledFragment(
            root, "wp357_seed_V" + std::to_string(level));
    }
    wp357WriteText(root / "passes" / "main.json",
                   wp357BloomConfig(hdr, peeled_stage).dump(2));
}

struct Wp357PhysicalFacts {
    std::string fragment_ref;
    std::string vertex_ref;
    bool descriptor_fragment_matches = false;
    ShaderBundleId vertex_shader{};
    std::optional<ShaderBundleId> fragment_shader;
    std::vector<FullscreenInputSampling> sampling;
    vk::AttachmentLoadOp load_op = vk::AttachmentLoadOp::eDontCare;
    std::size_t input_count = 0;
    bool input_target_matches = false;
    bool output_target_matches = false;
    std::vector<vk::Format> color_formats;
    std::vector<GraphicsPipelineColorAttachmentState> attachment_states;
    bool legacy_blend_enabled = false;
};

struct Wp357BloomCapture {
    bool hdr = false;
    std::optional<std::size_t> peeled_stage;
    std::uint32_t sub_texel_precision_bits = 0;
    std::array<RenderTargetReadback, 4> seeds;
    RenderTargetReadback working_v3;
    std::array<RenderTargetReadback, 3> stages;
    std::array<Wp357PhysicalFacts, 3> physical;
};

const CompiledPass &wp357FindCompiledPass(
    const CompiledRenderProgram &program,
    std::string_view name) {
    const auto found = std::find_if(
        program.rendering_pass.passes.begin(),
        program.rendering_pass.passes.end(),
        [&](const auto &pass) { return pass.definition.name == name; });
    if (found == program.rendering_pass.passes.end()) {
        throw std::runtime_error("WP357 compiled pass not found: " +
                                 std::string{name});
    }
    return *found;
}

Wp357BloomCapture wp357CaptureBloom(
    bool hdr, std::optional<std::size_t> peeled_stage) {
    const auto root = wp357TempProject(
        hdr ? "bloom_hdr" : "bloom_ldr");
    try {
        wp357WriteBloomProject(root, hdr, peeled_stage);
        Wp357BloomCapture capture{
            .hdr = hdr,
            .peeled_stage = peeled_stage,
        };
        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(root, false);
            GET_MODULE(ProjectSource).setProjectData(
                wp357ReadJson(root / "project.json").dump());
            auto &launch = GET_MODULE(EngineLaunchConfig);
            launch.headless = true;
            launch.shader_hot_reload = false;
            launch.headless_extent = wp357RenderExtent;
            launch.headless_frames = 1;
            auto &vkcore = GET_MODULE(VulkanManageCore);
            capture.sub_texel_precision_bits =
                vkcore.getPhysDevice().getProperties().limits
                    .subTexelPrecisionBits;
            auto &time = GET_MODULE(EngineTime);
            time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
            auto &renderer = GET_MODULE(Renderer);

            const auto rendering_pass_id =
                GET_MODULE(RenderingPassContainer)
                    .getRenderingPassIdByName("main_render");
            const auto program =
                GET_MODULE(FrameGraphRuntimeContainer)
                    .findProgram(rendering_pass_id);
            REQUIRE(program != nullptr);
            auto &fullscreen = GET_MODULE(FullscreenPassContainer);
            auto &targets = GET_MODULE(RenderTargetContainer);
            const auto expected_format =
                hdr ? vk::Format::eR16G16B16A16Sfloat
                    : vk::Format::eR8G8B8A8Srgb;

            for (std::size_t stage = 0; stage < 3; ++stage) {
                const auto &compiled =
                    wp357FindCompiledPass(*program, wp357PassNames[stage]);
                const auto desc =
                    fullscreen.graphicsPipelineDescForTesting(compiled.pass_id);
                const auto fragment =
                    fullscreen.fragmentShaderForTesting(compiled.pass_id);
                const auto destination_level = wp357DestinationLevels[stage];
                const auto source_level = destination_level + 1;
                capture.physical[stage] = {
                    .fragment_ref = compiled.definition.fullscreenInfo()
                                        .frag_shader.ref,
                    .vertex_ref = compiled.definition.fullscreenInfo()
                                      .vert_shader.ref,
                    .descriptor_fragment_matches =
                        desc.frag.has_value() && *desc.frag == fragment,
                    .vertex_shader = desc.vert,
                    .fragment_shader = desc.frag,
                    .sampling = fullscreen.inputSamplingForTesting(
                        compiled.pass_id),
                    .load_op = compiled.definition
                                   .colorAttachmentOperations(0)
                                   .load_op,
                    .input_count = compiled.definition.input_targets.size(),
                    .input_target_matches =
                        compiled.definition.input_targets.size() == 1 &&
                        compiled.definition.input_targets.front() ==
                            targets.getRenderTargetIdByName(
                                std::string{wp357LevelTargets[source_level]}),
                    .output_target_matches =
                        compiled.definition.output_color.size() == 1 &&
                        compiled.definition.output_color.front().target ==
                            targets.getRenderTargetIdByName(
                                std::string{
                                    wp357LevelTargets[destination_level]}),
                    .color_formats = desc.color_formats,
                    .attachment_states = desc.color_attachment_states,
                    .legacy_blend_enabled = desc.blend,
                };
                REQUIRE(capture.physical[stage].color_formats ==
                        std::vector<vk::Format>{expected_format});
            }

            time.advance();
            renderer.render();
            vkcore.waitIdle();
            for (std::size_t level = 0; level < 4; ++level) {
                capture.seeds[level] =
                    renderer.readRenderTargetForTesting(
                        "wp357_seed_V" + std::to_string(level),
                        ImageSubresourceRange{});
            }
            capture.working_v3 =
                renderer.readRenderTargetForTesting(
                    std::string{wp357LevelTargets[3]},
                    ImageSubresourceRange{});
            for (std::size_t stage = 0; stage < 3; ++stage) {
                capture.stages[stage] =
                    renderer.readRenderTargetForTesting(
                        std::string{wp357LevelTargets[
                            wp357DestinationLevels[stage]]},
                        ImageSubresourceRange{});
            }
        }
        std::filesystem::remove_all(root);
        return capture;
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
}

void wp357RequirePhysical(const Wp357BloomCapture &capture) {
    const auto expected_format =
        capture.hdr ? vk::Format::eR16G16B16A16Sfloat
                    : vk::Format::eR8G8B8A8Srgb;
    for (std::size_t stage = 0; stage < 3; ++stage) {
        INFO(wp357PassNames[stage]);
        const auto &facts = capture.physical[stage];
        CHECK(facts.fragment_ref == "engine://bloom_upsample");
        CHECK(facts.vertex_ref == "engine://fullscreen");
        CHECK(facts.descriptor_fragment_matches);
        CHECK(facts.input_count == 1);
        CHECK(facts.input_target_matches);
        CHECK(facts.output_target_matches);
        CHECK(facts.load_op == vk::AttachmentLoadOp::eLoad);
        REQUIRE(facts.sampling.size() == 1);
        CHECK(facts.sampling.front().filter ==
              FullscreenInputFilter::linear);
        CHECK(facts.sampling.front().address_mode ==
              FullscreenInputAddressMode::repeat);
        CHECK(facts.color_formats == std::vector<vk::Format>{expected_format});
        const bool peeled = capture.peeled_stage == stage;
        if (peeled) {
            CHECK(facts.attachment_states.empty());
            CHECK_FALSE(facts.legacy_blend_enabled);
            continue;
        }
        REQUIRE(facts.attachment_states.size() == 1);
        const auto &blend = facts.attachment_states.front();
        CHECK(blend.blend_enabled);
        CHECK(blend.source_color == vk::BlendFactor::eOne);
        CHECK(blend.destination_color == vk::BlendFactor::eOne);
        CHECK(blend.color_operation == vk::BlendOp::eAdd);
        CHECK(blend.source_alpha == vk::BlendFactor::eOne);
        CHECK(blend.destination_alpha == vk::BlendFactor::eOne);
        CHECK(blend.alpha_operation == vk::BlendOp::eAdd);
        CHECK(blend.write_mask ==
              (vk::ColorComponentFlagBits::eR |
               vk::ColorComponentFlagBits::eG |
               vk::ColorComponentFlagBits::eB |
               vk::ColorComponentFlagBits::eA));
    }
}

TestSupport::BloomUpsampleImageView wp357OracleView(
    const RenderTargetReadback &readback, bool hdr) {
    return {
        .storage = hdr
                       ? TestSupport::BloomUpsampleStorage::rgba16_sfloat
                       : TestSupport::BloomUpsampleStorage::rgba8_srgb,
        .width = readback.extent.width,
        .height = readback.extent.height,
        .bytes = std::span<const std::uint8_t>{readback.bytes},
    };
}

std::uint32_t wp357StorageAt(const RenderTargetReadback &readback,
                             bool hdr, std::uint32_t x,
                             std::uint32_t y, std::size_t channel) {
    const auto pixel =
        static_cast<std::size_t>(y) * readback.extent.width + x;
    if (!hdr) return readback.bytes.at(pixel * 4u + channel);
    const auto offset = pixel * 8u + channel * 2u;
    return static_cast<std::uint32_t>(readback.bytes.at(offset)) |
           (static_cast<std::uint32_t>(readback.bytes.at(offset + 1u)) << 8u);
}

struct Wp357OracleMetrics {
    std::uint32_t maximum_distance = 0;
    std::uint32_t allowance_at_maximum = 0;
    std::uint32_t minimum_margin =
        std::numeric_limits<std::uint32_t>::max();
    std::size_t checked_channels = 0;
    std::size_t edge_pixels = 0;
    std::size_t internal_pixels = 0;
};

Wp357OracleMetrics wp357RequireOracle(
    const Wp357BloomCapture &capture) {
    Wp357OracleMetrics metrics;
    const auto expected_format =
        capture.hdr ? vk::Format::eR16G16B16A16Sfloat
                    : vk::Format::eR8G8B8A8Srgb;
    for (std::size_t level = 0; level < 4; ++level) {
        INFO(level);
        REQUIRE(capture.seeds[level].format == expected_format);
        REQUIRE(capture.seeds[level].extent == wp357LevelExtents[level]);
        const auto &seed = capture.seeds[level];
        const auto first = wp357StorageAt(seed, capture.hdr, 0, 0, 0);
        const auto x_internal = wp357StorageAt(
            seed, capture.hdr, std::min(2u, seed.extent.width - 1u),
            std::min(1u, seed.extent.height - 1u), 0);
        const auto y_internal = wp357StorageAt(
            seed, capture.hdr, std::min(1u, seed.extent.width - 1u),
            std::min(2u, seed.extent.height - 1u), 0);
        const auto opposite = wp357StorageAt(
            seed, capture.hdr, seed.extent.width - 1u,
            seed.extent.height - 1u, 0);
        CHECK(first != x_internal);
        CHECK(first != y_internal);
        CHECK(first != opposite);
    }
    REQUIRE(capture.working_v3.format == expected_format);
    REQUIRE(capture.working_v3.extent == wp357LevelExtents[3]);
    REQUIRE(capture.working_v3.bytes == capture.seeds[3].bytes);

    for (std::size_t stage = 0; stage < 3; ++stage) {
        INFO(wp357PassNames[stage]);
        const auto destination_level = wp357DestinationLevels[stage];
        const auto &destination = capture.seeds[destination_level];
        const auto &source = stage == 0 ? capture.working_v3
                                        : capture.stages[stage - 1];
        const auto &actual = capture.stages[stage];
        REQUIRE(actual.format == expected_format);
        REQUIRE(actual.extent == wp357LevelExtents[destination_level]);
        for (std::uint32_t y = 0; y < actual.extent.height; ++y) {
            for (std::uint32_t x = 0; x < actual.extent.width; ++x) {
                const bool edge = x == 0 || y == 0 ||
                                  x + 1 == actual.extent.width ||
                                  y + 1 == actual.extent.height;
                if (edge) ++metrics.edge_pixels;
                else ++metrics.internal_pixels;
                const auto result =
                    TestSupport::evaluateBloomUpsampleOracle({
                        .destination = wp357OracleView(
                            destination, capture.hdr),
                        .source = wp357OracleView(source, capture.hdr),
                        .actual = wp357OracleView(actual, capture.hdr),
                        .sub_texel_precision_bits =
                            capture.sub_texel_precision_bits,
                        .pixel_center_x = static_cast<double>(x) + 0.5,
                        .pixel_center_y = static_cast<double>(y) + 0.5,
                        .address_mode = TestSupport::
                            BloomUpsampleAddressMode::repeat,
                        .blend_mode = capture.peeled_stage == stage
                            ? TestSupport::BloomUpsampleBlendMode::replace
                            : TestSupport::BloomUpsampleBlendMode::one_plus_one,
                    });
                if (!result.matches) {
                    CAPTURE(stage, x, y,
                            capture.sub_texel_precision_bits);
                    std::ostringstream detail;
                    for (std::size_t channel = 0; channel < 4; ++channel) {
                        const auto &value = result.channels[channel];
                        detail << " c" << channel
                               << " actual=" << value.actual_storage
                               << " ideal=" << value.ideal_storage
                               << " interval=[" << value.lower_storage
                               << ',' << value.upper_storage << ']'
                               << " filterBound="
                               << value.filter_bound_linear;
                    }
                    INFO(detail.str());
                    REQUIRE(result.matches);
                }
                for (const auto &channel : result.channels) {
                    ++metrics.checked_channels;
                    if (channel.storage_distance > metrics.maximum_distance) {
                        metrics.maximum_distance = channel.storage_distance;
                        metrics.allowance_at_maximum =
                            channel.storage_allowance;
                    }
                    metrics.minimum_margin = std::min(
                        metrics.minimum_margin, channel.storage_margin);
                }
            }
        }
    }
    REQUIRE(metrics.edge_pixels > 0);
    REQUIRE(metrics.internal_pixels > 0);
    REQUIRE(metrics.checked_channels > 0);
    return metrics;
}

std::size_t wp357ChangedPixels(const RenderTargetReadback &left,
                               const RenderTargetReadback &right,
                               bool hdr) {
    REQUIRE(left.format == right.format);
    REQUIRE(left.extent == right.extent);
    REQUIRE(left.bytes.size() == right.bytes.size());
    const auto stride = hdr ? 8u : 4u;
    std::size_t changed = 0;
    for (std::size_t offset = 0; offset < left.bytes.size(); offset += stride) {
        if (!std::equal(left.bytes.begin() + offset,
                        left.bytes.begin() + offset + stride,
                        right.bytes.begin() + offset)) {
            ++changed;
        }
    }
    return changed;
}

void wp357RunBloomFormat(bool hdr) {
    const auto canonical = wp357CaptureBloom(hdr, std::nullopt);
    wp357RequirePhysical(canonical);
    const auto canonical_metrics = wp357RequireOracle(canonical);
    std::cout << "WP357_ORACLE storage="
              << (hdr ? "R16G16B16A16_SFLOAT" : "R8G8B8A8_SRGB")
              << " variant=canonical subTexelPrecisionBits="
              << canonical.sub_texel_precision_bits
              << " maxDistance=" << canonical_metrics.maximum_distance
              << " allowanceAtMax="
              << canonical_metrics.allowance_at_maximum
              << " distanceToLimit="
              << (canonical_metrics.allowance_at_maximum -
                  canonical_metrics.maximum_distance)
              << " minMargin=" << canonical_metrics.minimum_margin
              << " checkedChannels=" << canonical_metrics.checked_channels
              << '\n';

    for (std::size_t peeled = 0; peeled < 3; ++peeled) {
        const auto mutant = wp357CaptureBloom(hdr, peeled);
        wp357RequirePhysical(mutant);
        const auto metrics = wp357RequireOracle(mutant);
        const auto changed = wp357ChangedPixels(
            canonical.stages[peeled], mutant.stages[peeled], hdr);
        REQUIRE(changed > 0);
        const auto &center = mutant.stages[peeled];
        const auto x = center.extent.width / 2u;
        const auto y = center.extent.height / 2u;
        std::cout << "WP357_MUTANT storage="
                  << (hdr ? "R16G16B16A16_SFLOAT" : "R8G8B8A8_SRGB")
                  << " removed=" << wp357PassNames[peeled]
                  << " resolvedBlend=disabled"
                  << " centerStorage=";
        for (std::size_t channel = 0; channel < 4; ++channel) {
            if (channel != 0) std::cout << ',';
            std::cout << wp357StorageAt(center, hdr, x, y, channel);
        }
        std::cout << " changedPixels=" << changed
                  << " maxDistance=" << metrics.maximum_distance
                  << " allowanceAtMax=" << metrics.allowance_at_maximum
                  << " minMargin=" << metrics.minimum_margin << '\n';
    }
}

std::string wp357ConstantShader(const std::array<int, 4> &codes) {
    std::ostringstream source;
    source << "#version 460\nlayout(location=0) out vec4 outColor;\n"
              "void main(){ outColor=vec4(";
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (channel != 0) source << ',';
        source << codes[channel] << ".0/255.0";
    }
    source << "); }\n";
    return source.str();
}

nlohmann::json wp357MinimalConfig(std::string_view load_op) {
    auto config = nlohmann::json::parse(R"json({
      "render_targets": [],
      "rendering_passes": [{
        "name": "wp357_minimal",
        "passes": [
          {
            "name": "A",
            "type": "fullscreen",
            "input": [],
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "project://shaders/wp357_destination"
            },
            "clear_color": [0.0, 0.0, 0.0, 0.0],
            "output": {"color": "display", "depth": null}
          },
          {
            "name": "B",
            "type": "fullscreen",
            "after": "A",
            "input": [],
            "shader": {
              "vertex": "engine://fullscreen",
              "fragment": "project://shaders/wp357_source"
            },
            "clear_color": [0.0, 0.0, 0.0, 0.0],
            "raster_state": {
              "color_attachments": [{
                "blend": {
                  "color": {"src": "one", "dst": "one", "op": "add"},
                  "alpha": {"src": "one", "dst": "one", "op": "add"}
                }
              }]
            },
            "output": {
              "color": [{"target": "display", "load_op": "Load"}],
              "depth": null
            }
          }
        ]
      }]
    })json");
    config["rendering_passes"][0]["passes"][1]["output"]
          ["color"][0]["load_op"] = load_op;
    return config;
}

std::array<std::uint8_t, 4> wp357CaptureMinimal(std::string_view load_op) {
    const auto root = wp357TempProject(
        load_op == "Load" ? "minimal_load" : "minimal_clear");
    try {
        wp357WriteProjectShell(root, "WP357 minimal Load", "wp357_minimal");
        wp357WriteText(root / "shaders" / "wp357_destination.frag",
                       wp357ConstantShader({32, 44, 56, 68}));
        wp357WriteText(root / "shaders" / "wp357_source.frag",
                       wp357ConstantShader({48, 56, 64, 72}));
        wp357InstallPrecompiledFragment(root, "wp357_destination");
        wp357InstallPrecompiledFragment(root, "wp357_source");
        wp357WriteText(root / "passes" / "main.json",
                       wp357MinimalConfig(load_op).dump(2));
        std::array<std::uint8_t, 4> result{};
        {
            FastModuleContainer modules;
            GET_MODULE(PathResolver).setup(root, false);
            GET_MODULE(ProjectSource).setProjectData(
                wp357ReadJson(root / "project.json").dump());
            auto &launch = GET_MODULE(EngineLaunchConfig);
            launch.headless = true;
            launch.shader_hot_reload = false;
            launch.headless_extent = vk::Extent2D{16, 16};
            launch.headless_frames = 1;
            auto &time = GET_MODULE(EngineTime);
            time.setup(EngineTime::Mode::fixed_step, 1.0 / 60.0);
            auto &renderer = GET_MODULE(Renderer);
            time.advance();
            renderer.render();
            auto &vkcore = GET_MODULE(VulkanManageCore);
            vkcore.waitIdle();
            const auto readback = renderer.readRenderTargetForTesting(
                "display", ImageSubresourceRange{});
            REQUIRE(readback.format == vk::Format::eB8G8R8A8Srgb);
            REQUIRE(readback.extent == vk::Extent2D{16, 16});
            const auto offset =
                (static_cast<std::size_t>(8) * readback.extent.width + 8u) *
                4u;
            std::copy_n(readback.bytes.begin() + offset, 4, result.begin());
        }
        std::filesystem::remove_all(root);
        return result;
    } catch (...) {
        std::filesystem::remove_all(root);
        throw;
    }
}

} // namespace

void GoldenHarness::runBloomUpsampleOracle() {
    setupLogger();
    try {
        wp357RunBloomFormat(false);
        wp357RunBloomFormat(true);
    } catch (const std::runtime_error &error) {
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "WP357 bloom upsample GPU oracle unavailable");
        throw;
    }
}

void GoldenHarness::runBloomLoadSemantics() {
    setupLogger();
    auto load_config = wp357MinimalConfig("Load");
    const auto clear_config = wp357MinimalConfig("Clear");
    load_config["rendering_passes"][0]["passes"][1]["output"]
               ["color"][0]["load_op"] = "Clear";
    REQUIRE(load_config == clear_config);
    try {
        const auto loaded = wp357CaptureMinimal("Load");
        const auto cleared = wp357CaptureMinimal("Clear");
        // The engine-owned display attachment is BGRA8 sRGB.  RGB blending is
        // linear and the attachment performs the terminal sRGB encode; alpha
        // remains linear.  These are therefore the exact stored BGRA codes.
        constexpr std::array<std::uint8_t, 4> expected_loaded{
            182, 168, 152, 140};
        constexpr std::array<std::uint8_t, 4> expected_cleared{
            137, 129, 120, 72};
        CHECK(loaded == expected_loaded);
        CHECK(cleared == expected_cleared);
        std::cout << "WP357_LOAD centerLoad="
                  << static_cast<int>(loaded[0]) << ','
                  << static_cast<int>(loaded[1]) << ','
                  << static_cast<int>(loaded[2]) << ','
                  << static_cast<int>(loaded[3])
                  << " centerClear="
                  << static_cast<int>(cleared[0]) << ','
                  << static_cast<int>(cleared[1]) << ','
                  << static_cast<int>(cleared[2]) << ','
                  << static_cast<int>(cleared[3]) << '\n';
    } catch (const std::runtime_error &error) {
        TestSupport::skipIfVulkanDeviceUnavailable(
            error, "WP357 minimal Load GPU fixture unavailable");
        throw;
    }
}

} // namespace Pelican
