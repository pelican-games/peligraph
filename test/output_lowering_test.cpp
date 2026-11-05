// VO-1a: output.file lowering, its refusal codes and the one plan-time
// last-writer rule, without a GPU.
#include "../src/core/output/outputfile.hpp"
#include "../src/core/output/outputreadback.hpp"
#include "../src/core/output/outputsinks.hpp"
#include "../src/core/watch/problemlist.hpp"
#include "../src/project/graphmodule.hpp"
#include "../src/project/output/outputlowering.hpp"
#include "../src/project/output/outputplanning.hpp"
#include "../src/project/renderconfigdocument.hpp"
#include "../src/project/renderpipeline.hpp"
#include "../src/project/targetrenderplanning.hpp"
#include "../src/core/renderingpass/renderingsamplecount.hpp"
#include "registered_render_targets_test_support.hpp"
#include "render_graph_ir_test_support.hpp"

#include <catch2/catch_test_macros.hpp>
#include <stb_image.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using Json = nlohmann::json;

Json sceneColorType() {
    return Json{{"semantic", "pelican.render.color_signal@1"},
                {"arguments",
                 {{"reference", "scene"},
                  {"transfer", "linear"},
                  {"range", "extended"}}}};
}

Json colorPort() {
    return Json{{"kind", "image"},
                {"type", sceneColorType()},
                {"access", "write"},
                {"attachment", "color"},
                {"location", 0}};
}

// A root graph with one 4x4 RGBA8 image, one fullscreen pass writing it, and a
// sink reading that pass's color port. `parameters` replaces the sink's
// parameters when given.
Json document(Json sink_parameters = Json{{"name", "shot"}}) {
    return Json{
        {"schema", "pelican.graph"},
        {"version", 1},
        {"definitions", Json::object()},
        {"rendering", {{"rendering_passes", Json::array({{{"name", "main_render"}}})}}},
        {"nodes",
         Json::array(
             {{{"id", "target"},
               {"op", "render.image"},
               {"parameters",
                {{"type", sceneColorType()},
                 {"format", "R8G8B8A8_UNORM"},
                 {"dimension", "2d"},
                 {"layers", 1},
                 {"extent_scale", 1.0},
                 {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}}},
               {"inputs", Json::object()},
               {"outputs", Json::array({"resource"})}},
              {{"id", "fill"},
               {"op", "render.fullscreen"},
               {"parameters",
                {{"pass_set", "main_render"},
                 {"ports", {{"color", colorPort()}}},
                 {"shader", {{"vertex", "engine://fullscreen"},
                             {"fragment", "engine://passthrough"}}}}},
               {"inputs", {{"color", {{"node", "target"}, {"port", "resource"}}}}},
               {"outputs", Json::array({"color"})}},
              {{"id", "sink"},
               {"op", "output.file"},
               {"parameters", std::move(sink_parameters)},
               {"inputs", {{"source", {{"node", "fill"}, {"port", "color"}}}}},
               {"outputs", Json::array()}}})},
        {"outputs", Json::object()}};
}

Json &node(Json &graph, std::string_view id) {
    for (auto &candidate : graph.at("nodes"))
        if (candidate.at("id").get<std::string>() == id) return candidate;
    throw std::runtime_error("missing fixture node");
}

const Json &node(const Json &graph, std::string_view id) {
    for (const auto &candidate : graph.at("nodes"))
        if (candidate.at("id").get<std::string>() == id) return candidate;
    throw std::runtime_error("missing fixture node");
}

std::string refusal(const Json &graph) {
    try {
        (void)Pelican::lowerOutputNodes(graph);
    } catch (const Pelican::GraphRegistryError &error) {
        return error.code();
    }
    return "accepted";
}

Pelican::TargetLoweringNode writer(std::string name, std::string target) {
    Pelican::TargetLoweringNode node;
    node.logical.name = std::move(name);
    node.logical.uses.push_back(Pelican::LogicalResourceUse{
        .port = "color",
        .access = Pelican::LogicalAccessMode::write,
        .output_value = Pelican::LogicalValueId{.resource = std::move(target), .version = 0},
    });
    return node;
}

Pelican::OutputSinkTable sinkTable() {
    Pelican::OutputSinkTable table;
    table.sinks.push_back(Pelican::OutputSinkSpec{
        .name = "shot",
        .kind = "file",
        .graph = "main_render",
        .target = "target",
        .source_node = "fill",
        .source_port = "color",
    });
    return table;
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::vector<std::uint8_t> pattern() {
    std::vector<std::uint8_t> pixels(4 * 4 * 4);
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto at = (y * 4 + x) * 4;
            pixels[at + 0] = static_cast<std::uint8_t>(10 + x);
            pixels[at + 1] = static_cast<std::uint8_t>(20 + y);
            pixels[at + 2] = static_cast<std::uint8_t>(30 + x * y);
            pixels[at + 3] = static_cast<std::uint8_t>(40 + x + y);
        }
    }
    return pixels;
}

std::vector<std::uint8_t> bgraBytes(std::vector<std::uint8_t> rgba) {
    for (std::size_t at = 0; at + 3 < rgba.size(); at += 4)
        std::swap(rgba[at], rgba[at + 2]);
    return rgba;
}

struct DecodedPng {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<std::uint8_t> rgba;
};

DecodedPng decodePng(const std::filesystem::path &path) {
    const auto bytes = readBytes(path);
    REQUIRE(bytes.size() <=
            static_cast<std::size_t>((std::numeric_limits<int>::max)()));
    int width = 0;
    int height = 0;
    int channels = 0;
    auto *pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc *>(bytes.data()),
        static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    REQUIRE(pixels != nullptr);
    DecodedPng png;
    png.width = width;
    png.height = height;
    png.channels = channels;
    png.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) *
                                         height * 4);
    stbi_image_free(pixels);
    return png;
}

bool pngHasChunk(const std::vector<std::uint8_t> &bytes,
                 std::string_view type) {
    if (bytes.size() < 8) return false;
    std::size_t at = 8;
    while (at + 12 <= bytes.size()) {
        const auto length = (static_cast<std::uint32_t>(bytes.at(at)) << 24) |
                            (static_cast<std::uint32_t>(bytes.at(at + 1)) << 16) |
                            (static_cast<std::uint32_t>(bytes.at(at + 2)) << 8) |
                            static_cast<std::uint32_t>(bytes.at(at + 3));
        if (at + 12 + length > bytes.size()) return false;
        if (std::string_view{
                reinterpret_cast<const char *>(bytes.data() + at + 4), 4} ==
            type)
            return true;
        at += 12 + length;
    }
    return false;
}

std::uint32_t le32(const std::vector<std::uint8_t> &bytes, std::size_t at) {
    return static_cast<std::uint32_t>(bytes.at(at)) |
           (static_cast<std::uint32_t>(bytes.at(at + 1)) << 8) |
           (static_cast<std::uint32_t>(bytes.at(at + 2)) << 16) |
           (static_cast<std::uint32_t>(bytes.at(at + 3)) << 24);
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        path = std::filesystem::temp_directory_path() /
               ("pelican-vo1a-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(std::filesystem::create_directories(path));
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

} // namespace

TEST_CASE("VO-1a the module registers output.file as a sink", "[vo1a][output-module]") {
    const auto &registry = Pelican::builtinGraphModuleRegistry();
    CHECK(registry.contains("output.file"));
    CHECK(registry.isSink("output.file"));
    const auto catalog = Json(registry.catalog());
    bool module_found = false;
    bool operation_found = false;
    bool sink_found = false;
    for (const auto &module : catalog.at("modules")) {
        if (module.at("namespace") != "pelican.output") continue;
        module_found = true;
        for (const auto &operation : module.at("operations"))
            if (operation.at("op") == "output.file") operation_found = true;
        for (const auto &sink : module.at("sinks"))
            if (sink == "output.file") sink_found = true;
    }
    CHECK(module_found);
    CHECK(operation_found);
    CHECK(sink_found);
}

TEST_CASE("VO-1a the file is a PVO1 header and tightly packed row 0 first", "[vo1a][output-file]") {
    TemporaryDirectory directory;
    const auto pixels = pattern();
    const auto written = Pelican::writeOutputFrameFile(
        directory.path, "shot", 7,
        Pelican::OutputFileImage{.width = 4, .height = 4,
                                 .format = Pelican::OutputFileFormat::rgba8_unorm,
                                 .pixels = pixels});
    REQUIRE(written.status == Pelican::OutputFileWriteStatus::written);
    const auto path = directory.path / "shot_000007.rgba";
    REQUIRE(std::filesystem::is_regular_file(path));
    const auto bytes = readBytes(path);
    REQUIRE(bytes.size() == 16 + pixels.size());
    CHECK(bytes.at(0) == 'P');
    CHECK(bytes.at(1) == 'V');
    CHECK(bytes.at(2) == 'O');
    CHECK(bytes.at(3) == '1');
    CHECK(le32(bytes, 4) == 4);
    CHECK(le32(bytes, 8) == 4);
    CHECK(le32(bytes, 12) == 0);
    CHECK(std::vector<std::uint8_t>(bytes.begin() + 16, bytes.end()) == pixels);
    CHECK_FALSE(std::filesystem::exists(directory.path / "shot_000007.rgba.partial"));
}

TEST_CASE("VO-1a an existing final file is never replaced and a partial is never published", "[vo1a][output-file]") {
    TemporaryDirectory directory;
    const auto pixels = pattern();
    const auto image = Pelican::OutputFileImage{.width = 4, .height = 4,
                                                .format = Pelican::OutputFileFormat::bgra8_srgb,
                                                .pixels = pixels};
    REQUIRE(Pelican::writeOutputFrameFile(directory.path, "shot", 1, image).status ==
            Pelican::OutputFileWriteStatus::written);
    const auto final_path = directory.path / "shot_000001.rgba";
    const auto before = readBytes(final_path);
    const auto again = Pelican::writeOutputFrameFile(directory.path, "shot", 1, image);
    CHECK(again.status == Pelican::OutputFileWriteStatus::exists);
    CHECK(readBytes(final_path) == before);

    const auto partial = directory.path / "shot_000002.rgba.partial";
    {
        std::ofstream out{partial, std::ios::binary};
        out << "not mine";
    }
    const auto blocked = Pelican::writeOutputFrameFile(directory.path, "shot", 2, image);
    CHECK(blocked.status == Pelican::OutputFileWriteStatus::failed);
    CHECK_FALSE(std::filesystem::exists(directory.path / "shot_000002.rgba"));
}

TEST_CASE("VO-1a get_frame_plan reports readback after lowering", "[vo1a][output-lowering]") {
    const auto lowered = Pelican::lowerOutputNodes(document());
    const auto resolved = Pelican::resolveRenderPipeline(
        Pelican::RenderPipelineRequest{.authored_config = lowered.document,
                                       .source_name = "vo1a frame plan"},
        Pelican::RenderEnvironmentCapabilities{
            .runtime_shader_compiler_enabled = true,
            .graph_variant = Pelican::RenderPipelineGraphVariant::flat});
    const auto compiled = Pelican::compileRenderPipeline(resolved);
    const auto render_graph = Pelican::TestSupport::renderGraphOf(resolved.composed_config);
    REQUIRE(render_graph.graphs.size() == 1);
    const auto targets = Pelican::test_support::renderTargetDefinitionsOf(resolved.composed_config);
    const auto compilation = Pelican::compileRenderingTargetPlans(
        render_graph, Pelican::TestSupport::framePlanMapOf(render_graph), targets,
        compiled.sample_count_policy, vk::Format::eB8G8R8A8Srgb,
        Pelican::RenderingTargetPlanDeviceFacts{
            .multiview = true,
            .max_multiview_view_count = 8,
            .query_attachment_samples =
                [](const Pelican::RenderTargetDefinition &) {
                    return std::vector<std::uint32_t>{1};
                },
            .query_image_format_capability =
                [](const Pelican::RenderTargetDefinition &) {
                    return Pelican::RenderingImageFormatCapability{
                        .image_usage_supported = true,
                        .supported_samples = {1},
                        .max_mip_levels = 16,
                        .max_array_layers = 8,
                        .transient_attachment_supported = true,
                        .local_read_attachment_supported = true,
                    };
                },
            .transient_attachments = true,
            .dynamic_rendering_local_read = true,
        },
        std::nullopt, std::nullopt, compiled.target_planning);
    REQUIRE(compilation.plans.size() == 1);
    // This is the plan get_frame_plan publishes as physical_target_plan.
    const auto physical = Pelican::vulkanTargetPlanToJson(*compilation.plans.front());
    REQUIRE(physical.contains("resources"));
    const auto resource = std::find_if(
        physical.at("resources").begin(), physical.at("resources").end(),
        [](const Json &candidate) {
            return candidate.at("logical_resource") == "target";
        });
    REQUIRE(resource != physical.at("resources").end());
    CHECK(resource->at("lifetime").at("retain") == Json::array({"readback"}));
}

TEST_CASE("VO-1a output.file lowers to a report and a retained target", "[vo1a][output-lowering]") {
    const auto authored = document();
    const auto authored_text = authored.dump();
    const auto lowered = Pelican::lowerOutputNodes(authored);

    CHECK(authored.dump() == authored_text);
    CHECK(lowered.changed);
    CHECK(lowered.document.at("nodes").size() == 2);
    for (const auto &remaining : lowered.document.at("nodes"))
        CHECK(remaining.at("op") != "output.file");
    // The sink's edge goes with the node; the pass keeps its own edge.
    CHECK_FALSE(node(lowered.document, "fill").at("inputs").contains("source"));
    CHECK(node(lowered.document, "fill").at("inputs").at("color") ==
          Json{{"node", "target"}, {"port", "resource"}});

    CHECK(lowered.report ==
          Json{{"sinks", Json::array({{{"name", "shot"},
                                       {"kind", "file"},
                                       {"every_n", 1},
                                       {"graph", "main_render"},
                                       {"target", "target"},
                                       {"format", "raw"}}})}});
    REQUIRE(lowered.sinks.sinks.size() == 1);
    const auto &sink = lowered.sinks.sinks.front();
    CHECK(sink.name == "shot");
    CHECK(sink.kind == "file");
    CHECK(sink.every_n == 1);
    CHECK(sink.enabled);
    CHECK(sink.graph == "main_render");
    CHECK(sink.target == "target");
    CHECK(sink.source_node == "fill");
    CHECK(sink.source_port == "color");
    CHECK(sink.error_code.empty());
    CHECK(sink.format == "raw");
    CHECK(lowered.document.at("rendering").at("target_planning").at("graphs").at("main_render")
              .at("resources").at("target").at("retain") == true);
}

TEST_CASE("VO-1a output.file keeps every_n and a target_ref names an external target", "[vo1a][output-lowering]") {
    auto graph = document(Json{{"name", "takes"}, {"every_n", 3}});
    node(graph, "target") = Json{{"id", "external"},
                                 {"op", "render.target_ref"},
                                 {"parameters", {{"name", "external_color"},
                                                  {"type", sceneColorType()}}},
                                 {"inputs", Json::object()},
                                 {"outputs", Json::array({"resource"})}};
    node(graph, "fill").at("inputs").at("color") = Json{{"node", "external"}, {"port", "resource"}};
    const auto lowered = Pelican::lowerOutputNodes(graph);
    REQUIRE(lowered.report.at("sinks").size() == 1);
    CHECK(lowered.report.at("sinks").at(0).at("every_n") == 3);
    CHECK(lowered.report.at("sinks").at(0).at("target") == "external_color");
    CHECK(lowered.document.at("rendering").at("target_planning").at("graphs").at("main_render")
              .at("resources").at("external_color").at("retain") == true);
}

TEST_CASE("VO-1a every_n is not truncated to 32 bits", "[vo1a][output-lowering]") {
    const auto lowered = Pelican::lowerOutputNodes(
        document(Json{{"name", "wide"}, {"every_n", 4294967296ULL}}));
    REQUIRE(lowered.report.at("sinks").size() == 1);
    CHECK(lowered.report.at("sinks").at(0).at("every_n") == 4294967296ULL);
    REQUIRE(lowered.sinks.sinks.size() == 1);
    CHECK(lowered.sinks.sinks.at(0).every_n == 4294967296ULL);
}

TEST_CASE("VO-1a a disabled sink leaves no report row and no retain", "[vo1a][output-lowering]") {
    const auto lowered =
        Pelican::lowerOutputNodes(document(Json{{"name", "off"}, {"enabled", false}}));
    CHECK(lowered.changed);
    CHECK(lowered.document.at("nodes").size() == 2);
    CHECK(lowered.report == Json{{"sinks", Json::array()}});
    CHECK(lowered.sinks.sinks.empty());
    const bool has_planning =
        lowered.document.contains("rendering") &&
        lowered.document.at("rendering").contains("target_planning");
    CHECK_FALSE(has_planning);
}

TEST_CASE("VO-1a a document without outputs is unchanged", "[vo1a][output-lowering]") {
    auto graph = document();
    graph.at("nodes").erase(graph.at("nodes").begin() + 2);
    const auto lowered = Pelican::lowerOutputNodes(graph);
    CHECK_FALSE(lowered.changed);
    CHECK(lowered.document == graph);
    CHECK(lowered.report == Json{{"sinks", Json::array()}});
    CHECK(lowered.sinks.sinks.empty());
}

TEST_CASE("VO-1a output.file refuses names and parameters", "[vo1a][output-lowering]") {
    SECTION("name syntax") {
        for (const auto *name : {"", "Upper", "1st", "has-dash", "has space"}) {
            INFO(name);
            auto graph = document(Json{{"name", name}});
            CHECK(refusal(graph) == "output.name_invalid@1");
        }
    }
    SECTION("every_n is a positive integer") {
        for (const auto &every : Json::array({0, -1, "2", 1.5, nullptr})) {
            INFO(every.dump());
            auto graph = document(Json{{"name", "shot"}, {"every_n", every}});
            CHECK(refusal(graph) == "output.parameter_invalid@1");
        }
    }
    SECTION("enabled is a bool") {
        auto graph = document(Json{{"name", "shot"}, {"enabled", 1}});
        CHECK(refusal(graph) == "output.parameter_invalid@1");
    }
    SECTION("an unknown parameter is refused") {
        auto graph = document(Json{{"name", "shot"}, {"colour_space", "srgb"}});
        CHECK(refusal(graph) == "output.parameter_invalid@1");
    }
    SECTION("format accepts raw and png only") {
        for (const auto &format : Json::array({"raw", "png"})) {
            INFO(format.dump());
            auto graph = document(Json{{"name", "shot"}, {"format", format}});
            const auto lowered = Pelican::lowerOutputNodes(graph);
            REQUIRE(lowered.report.at("sinks").size() == 1);
            CHECK(lowered.report.at("sinks").at(0).at("format") == format);
            REQUIRE(lowered.sinks.sinks.size() == 1);
            CHECK(lowered.sinks.sinks.front().format == format.get<std::string>());
        }
        for (const auto &format : Json::array({1, true, nullptr, "PNG",
                                                "R8G8B8A8_UNORM", "Raw"})) {
            INFO(format.dump());
            auto graph = document(Json{{"name", "shot"}, {"format", format}});
            CHECK(refusal(graph) == "output.file_format_unknown@1");
        }
    }
    SECTION("the absent format is the raw format") {
        const auto implicit = Pelican::lowerOutputNodes(document());
        const auto explicit_raw = Pelican::lowerOutputNodes(
            document(Json{{"name", "shot"}, {"format", "raw"}}));
        CHECK(implicit.report == explicit_raw.report);
        CHECK(implicit.sinks.sinks.front().format == "raw");
        CHECK(Pelican::renderConfigSourceDigest(implicit.document.dump()) ==
              "c8ab8e70722b8dd8595980c3daf64a85b900b3fe776b69a2292ab6d592a12841");
    }
    SECTION("names are unique") {
        auto graph = document();
        auto second = graph.at("nodes").at(2);
        second.at("id") = "sink_too";
        graph.at("nodes").push_back(std::move(second));
        CHECK(refusal(graph) == "output.name_duplicate@1");
    }
}

TEST_CASE("VO-2 when_full defaults to wait without changing the document digest",
          "[vo2][output-lowering]") {
    const auto implicit = Pelican::lowerOutputNodes(document());
    const auto explicit_wait = Pelican::lowerOutputNodes(
        document(Json{{"name", "shot"}, {"when_full", "wait"}}));
    CHECK(implicit.sinks.sinks.front().when_full == "wait");
    CHECK(explicit_wait.sinks.sinks.front().when_full == "wait");
    CHECK(implicit.report == explicit_wait.report);
    CHECK(implicit.document == explicit_wait.document);
    CHECK(Pelican::renderConfigSourceDigest(implicit.document.dump()) ==
          "c8ab8e70722b8dd8595980c3daf64a85b900b3fe776b69a2292ab6d592a12841");
    const auto drop = Pelican::lowerOutputNodes(
        document(Json{{"name", "shot"}, {"when_full", "drop"}}));
    CHECK(drop.sinks.sinks.front().when_full == "drop");
    for (const auto value : Json::array({"block", "DROP", 1, true, nullptr})) {
        INFO(value.dump());
        CHECK(refusal(document(Json{{"name", "shot"}, {"when_full", value}})) ==
              "output.when_full_unknown@1");
    }
}

TEST_CASE("VO-1a output.file resolves its source to a graph and target", "[vo1a][output-lowering]") {
    SECTION("an unconnected sink") {
        auto graph = document();
        node(graph, "sink").erase("inputs");
        CHECK(refusal(graph) == "output.source_invalid@1");
    }
    SECTION("a missing source edge") {
        auto graph = document();
        node(graph, "sink").at("inputs") = Json::object();
        CHECK(refusal(graph) == "output.source_invalid@1");
    }
    SECTION("a source that is not a pass") {
        auto graph = document();
        node(graph, "sink").at("inputs").at("source") = Json{{"node", "target"}, {"port", "resource"}};
        CHECK(refusal(graph) == "output.source_invalid@1");
    }
    SECTION("a pass port that is not written") {
        auto graph = document();
        node(graph, "sink").at("inputs").at("source") = Json{{"node", "fill"}, {"port", "resource"}};
        CHECK(refusal(graph) == "output.source_invalid@1");
    }
    SECTION("a pass input that is not an image") {
        auto graph = document();
        node(graph, "target") = Json{{"id", "buffer"},
                                     {"op", "render.buffer"},
                                     {"parameters", {{"element", "f32"}, {"count", 4}}},
                                     {"inputs", Json::object()},
                                     {"outputs", Json::array({"resource"})}};
        node(graph, "fill").at("inputs").at("color") = Json{{"node", "buffer"}, {"port", "resource"}};
        CHECK(refusal(graph) == "output.source_invalid@1");
    }
    SECTION("a target whose declared type is not scene linear extended") {
        auto graph = document();
        node(graph, "target").at("parameters").at("type") =
            Json{{"semantic", "pelican.render.depth@1"},
                 {"arguments", {{"representation", "device"}, {"space", "projection"}}}};
        CHECK(refusal(graph) == "output.source_invalid@1");
    }
    SECTION("a target with no declared type") {
        auto graph = document();
        node(graph, "target").at("parameters").erase("type");
        CHECK(refusal(graph) == "output.source_invalid@1");
    }
    SECTION("the same producer writing one target through two ports") {
        auto graph = document();
        auto &ports = node(graph, "fill").at("parameters").at("ports");
        ports["aux"] = colorPort();
        node(graph, "fill").at("inputs")["aux"] = Json{{"node", "target"}, {"port", "resource"}};
        node(graph, "fill").at("outputs").push_back("aux");
        CHECK(refusal(graph) == "output.source_ambiguous@1");
    }
}

TEST_CASE("VO-1a formats and layouts are runtime refusals, not lowering errors", "[vo1a][output-lowering]") {
    // The plan knows the target's effective format and shape, but the sink
    // must refuse those at readback without stopping compilation. Lowering
    // keeps the sink and its retain declaration in every case here.
    const auto lowered_ok = [](const Json &graph) {
        const auto lowered = Pelican::lowerOutputNodes(graph);
        REQUIRE(lowered.report.at("sinks").size() == 1);
        CHECK(lowered.document.at("rendering").at("target_planning").at("graphs")
                  .at("main_render").at("resources").at("target").at("retain") == true);
    };
    SECTION("an unsupported format") {
        auto graph = document();
        node(graph, "target").at("parameters").at("format") = "R16G16B16A16_SFLOAT";
        lowered_ok(graph);
    }
    SECTION("layers") {
        auto graph = document();
        node(graph, "target").at("parameters").at("layers") = 2;
        lowered_ok(graph);
    }
    SECTION("mip levels") {
        auto graph = document();
        node(graph, "target").at("parameters")["mip_levels"] = 2;
        lowered_ok(graph);
    }
    SECTION("dimension") {
        auto graph = document();
        node(graph, "target").at("parameters").at("dimension") = "cube";
        lowered_ok(graph);
    }
    SECTION("a sink inside a definition") {
        auto graph = document();
        graph["definitions"]["helper"] = Json{{"nodes", Json::array({{{"id", "nested"}, {"op", "output.file"}}})}};
        CHECK(refusal(graph) == "output.scope_unsupported@1");
    }
}

TEST_CASE("VO-1a the header names exactly the four byte formats", "[vo1a][output-file]") {
    const auto expect = [](vk::Format value, Pelican::OutputFileFormat expected) {
        const auto mapped = Pelican::outputFileFormat(value);
        CHECK(mapped.has_value());
        if (mapped) CHECK(*mapped == expected);
    };
    expect(vk::Format::eR8G8B8A8Unorm, Pelican::OutputFileFormat::rgba8_unorm);
    expect(vk::Format::eR8G8B8A8Srgb, Pelican::OutputFileFormat::rgba8_srgb);
    expect(vk::Format::eB8G8R8A8Unorm, Pelican::OutputFileFormat::bgra8_unorm);
    expect(vk::Format::eB8G8R8A8Srgb, Pelican::OutputFileFormat::bgra8_srgb);
    CHECK_FALSE(Pelican::outputFileFormat(vk::Format::eR16G16B16A16Sfloat).has_value());
    CHECK_FALSE(Pelican::outputFileFormat(vk::Format::eR8Unorm).has_value());
}

TEST_CASE("VO-1a at most eight sinks in a document", "[vo1a][output-lowering]") {
    auto graph = document();
    graph.at("nodes").erase(graph.at("nodes").begin() + 2);
    for (int index = 0; index < 8; ++index) {
        graph.at("nodes").push_back(
            Json{{"id", "sink" + std::to_string(index)},
                 {"op", "output.file"},
                 {"parameters", {{"name", "sink" + std::to_string(index)}}},
                 {"inputs", {{"source", {{"node", "fill"}, {"port", "color"}}}}},
                 {"outputs", Json::array()}});
    }
    const auto eight = Pelican::lowerOutputNodes(graph);
    CHECK(eight.report.at("sinks").size() == 8);

    // A disabled sink is still a node in the document and counts toward the
    // limit; it is not silently ignored by the table bound.
    auto ninth_disabled = graph;
    ninth_disabled.at("nodes").push_back(
        Json{{"id", "sink8"},
             {"op", "output.file"},
             {"parameters", {{"name", "sink8"}, {"enabled", false}}},
             {"inputs", {{"source", {{"node", "fill"}, {"port", "color"}}}}},
             {"outputs", Json::array()}});
    CHECK(refusal(ninth_disabled) == "output.too_many_sinks@1");

    graph.at("nodes").push_back(
        Json{{"id", "sink8"},
             {"op", "output.file"},
             {"parameters", {{"name", "sink8"}}},
             {"inputs", {{"source", {{"node", "fill"}, {"port", "color"}}}}},
             {"outputs", Json::array()}});
    CHECK(refusal(graph) == "output.too_many_sinks@1");
}

TEST_CASE("VO-1a a sink graph is exactly the compiled plan graph", "[vo1a][output-lowering]") {
    CHECK(Pelican::outputSinkGraphMatches("main_render", "main_render"));
    CHECK_FALSE(Pelican::outputSinkGraphMatches("main_render", "main"));
    CHECK_FALSE(Pelican::outputSinkGraphMatches("", ""));
    CHECK_FALSE(Pelican::outputSinkGraphMatches("", "main"));
}

TEST_CASE("VO-1a the sink's node must be the last writer of its target", "[vo1a][output-planning]") {
    Pelican::VulkanTargetPlan plan;
    plan.graph = "main_render";
    plan.lowering_graph.nodes = {writer("fill", "target"), writer("later", "target")};
    plan.scopes.push_back(Pelican::VulkanPhysicalScopePlan{.id = "scope", .nodes = {"fill", "later"}});

    auto table = sinkTable();
    Pelican::validateOutputSinkWriters(table, plan);
    CHECK(table.sinks.at(0).error_code == "output.source_overwritten@1");
    CHECK(table.sinks.at(0).error_message.find("later") != std::string::npos);

    auto reversed = plan;
    reversed.lowering_graph.nodes = {writer("later", "target"), writer("fill", "target")};
    reversed.scopes.at(0).nodes = {"later", "fill"};
    auto accepted = sinkTable();
    Pelican::validateOutputSinkWriters(accepted, reversed);
    CHECK(accepted.sinks.at(0).error_code.empty());

    auto other_graph = plan;
    other_graph.graph = "main";
    auto elsewhere = sinkTable();
    Pelican::validateOutputSinkWriters(elsewhere, other_graph);
    CHECK(elsewhere.sinks.at(0).error_code.empty());
}

TEST_CASE("VO-1a the writer check follows physical source nodes", "[vo1a][output-planning]") {
    Pelican::VulkanTargetPlan plan;
    plan.graph = "main_render";
    auto split = writer("fill$split", "target");
    split.source_nodes = {"fill"};
    plan.lowering_graph.nodes = {std::move(split), writer("later", "target")};
    plan.scopes.push_back(Pelican::VulkanPhysicalScopePlan{.id = "scope", .nodes = {"fill$split", "later"}});
    auto table = sinkTable();
    Pelican::validateOutputSinkWriters(table, plan);
    CHECK(table.sinks.at(0).error_code == "output.source_overwritten@1");
}

TEST_CASE("VO-1b PNG output keeps the four straight channels and row 0 first",
          "[vo1b][output-file]") {
    TemporaryDirectory directory;
    const auto pixels = pattern();

    SECTION("RGBA8 UNORM and RGBA8 SRGB write the same bytes") {
        std::uint64_t frame = 3;
        for (const auto format : {Pelican::OutputFileFormat::rgba8_unorm,
                                  Pelican::OutputFileFormat::rgba8_srgb}) {
            INFO(static_cast<int>(format));
            const auto written = Pelican::writeOutputFrameFile(
                directory.path, "shot", frame,
                Pelican::OutputFileImage{.width = 4,
                                         .height = 4,
                                         .format = format,
                                         .pixels = pixels},
                Pelican::OutputFileEncoding::png);
            REQUIRE(written.status == Pelican::OutputFileWriteStatus::written);
            const auto path = directory.path /
                              ("shot_" + std::to_string(1000000 + frame).substr(1) +
                               ".png");
            REQUIRE(std::filesystem::is_regular_file(path));
            CHECK(written.bytes_written ==
                  std::filesystem::file_size(path));
            const auto bytes = readBytes(path);
            REQUIRE(bytes.size() >= 26);
            CHECK(bytes.at(25) == 6);
            CHECK_FALSE(pngHasChunk(bytes, "sRGB"));
            CHECK_FALSE(pngHasChunk(bytes, "gAMA"));
            const auto png = decodePng(path);
            CHECK(png.width == 4);
            CHECK(png.height == 4);
            CHECK(png.rgba == pixels);
            CHECK_FALSE(std::filesystem::exists(
                directory.path /
                ("shot_" + std::to_string(1000000 + frame).substr(1) +
                 ".png.partial")));
            ++frame;
        }
    }

    SECTION("a BGRA8 readback is swapped to RGBA, not reinterpreted") {
        std::uint64_t frame = 4;
        for (const auto format : {Pelican::OutputFileFormat::bgra8_unorm,
                                  Pelican::OutputFileFormat::bgra8_srgb}) {
            const auto written = Pelican::writeOutputFrameFile(
                directory.path, "shot", frame,
                Pelican::OutputFileImage{.width = 4,
                                         .height = 4,
                                         .format = format,
                                         .pixels = bgraBytes(pixels)},
                Pelican::OutputFileEncoding::png);
            REQUIRE(written.status == Pelican::OutputFileWriteStatus::written);
            const auto path = directory.path /
                              ("shot_00000" + std::to_string(frame) + ".png");
            const auto bytes = readBytes(path);
            CHECK_FALSE(pngHasChunk(bytes, "sRGB"));
            CHECK_FALSE(pngHasChunk(bytes, "gAMA"));
            CHECK(decodePng(path).rgba == pixels);
            ++frame;
        }
    }
}

TEST_CASE("VO-1b a PNG budget is the real encoded size, and a refused file leaves nothing",
          "[vo1b][output-file]") {
    TemporaryDirectory directory;
    const auto pixels = pattern();
    const Pelican::OutputFileImage image{.width = 4,
                                         .height = 4,
                                         .format =
                                             Pelican::OutputFileFormat::rgba8_unorm,
                                         .pixels = pixels};
    const auto probe = Pelican::writeOutputFrameFile(
        directory.path, "probe", 0, image, Pelican::OutputFileEncoding::png);
    REQUIRE(probe.status == Pelican::OutputFileWriteStatus::written);
    REQUIRE(probe.bytes_written > 0);
    const auto encoded_size = probe.bytes_written;
    CHECK(encoded_size ==
          std::filesystem::file_size(directory.path / "probe_000000.png"));

    const auto exact = Pelican::writeOutputFrameFile(
        directory.path, "shot", 1, image, Pelican::OutputFileEncoding::png,
        encoded_size);
    REQUIRE(exact.status == Pelican::OutputFileWriteStatus::written);
    CHECK(exact.bytes_written == encoded_size);

    const auto short_budget = Pelican::writeOutputFrameFile(
        directory.path, "shot", 2, image, Pelican::OutputFileEncoding::png,
        encoded_size - 1);
    CHECK(short_budget.status ==
          Pelican::OutputFileWriteStatus::limit_reached);
    CHECK(short_budget.bytes_written == 0);
    CHECK(short_budget.message.find("needs") != std::string::npos);
    CHECK(short_budget.message.find("remain") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(directory.path / "shot_000002.png"));
    CHECK_FALSE(std::filesystem::exists(
        directory.path / "shot_000002.png.partial"));

    const auto accumulated = Pelican::writeOutputFrameFile(
        directory.path, "shot", 3, image, Pelican::OutputFileEncoding::png,
        encoded_size);
    REQUIRE(accumulated.status == Pelican::OutputFileWriteStatus::written);
    const auto exhausted = Pelican::writeOutputFrameFile(
        directory.path, "shot", 4, image, Pelican::OutputFileEncoding::png, 0);
    CHECK(exhausted.status == Pelican::OutputFileWriteStatus::limit_reached);
    CHECK_FALSE(std::filesystem::exists(directory.path / "shot_000004.png"));

    const auto raw_probe = Pelican::writeOutputFrameFile(
        directory.path, "raw_probe", 0, image);
    REQUIRE(raw_probe.status == Pelican::OutputFileWriteStatus::written);
    CHECK(raw_probe.bytes_written == 16 + pixels.size());
    const auto raw_exact = Pelican::writeOutputFrameFile(
        directory.path, "raw_shot", 1, image, Pelican::OutputFileEncoding::raw,
        raw_probe.bytes_written);
    REQUIRE(raw_exact.status == Pelican::OutputFileWriteStatus::written);
    const auto raw_short = Pelican::writeOutputFrameFile(
        directory.path, "raw_shot", 2, image, Pelican::OutputFileEncoding::raw,
        raw_probe.bytes_written - 1);
    CHECK(raw_short.status == Pelican::OutputFileWriteStatus::limit_reached);
    CHECK_FALSE(std::filesystem::exists(directory.path / "raw_shot_000002.rgba"));
}

TEST_CASE("VO-1b one problem per refused or limit-stopped sink, keyed by graph and sink",
          "[vo1b][output-problem]") {
    using Pelican::OutputSinks;

    OutputSinks::State refused;
    refused.name = "shot";
    refused.kind = "file";
    refused.graph = "main_render";
    refused.state = "refused";
    refused.last_error = "output.write_failed@1: /tmp/shot_000000.png.partial: file exists";

    OutputSinks::State limited;
    limited.name = "shot";
    limited.kind = "file";
    limited.graph = "other_render";
    limited.state = "finished";
    limited.stop_reason = "max_frames";
    limited.stop_value = 7;

    const auto problems =
        Pelican::outputSinkProblems({refused, limited});
    REQUIRE(problems.size() == 2);
    CHECK(problems.at(0).at("code") == "output.write_failed@1");
    CHECK(problems.at(0).at("severity") == "error");
    CHECK(problems.at(0).at("source") == "runtime");
    CHECK(problems.at(0).at("scope") == "main_render");
    CHECK(problems.at(0).at("node") == "shot");
    CHECK(problems.at(0).at("fields").at("graph") == "main_render");
    CHECK(problems.at(0).at("fields").at("sink") == "shot");
    CHECK(problems.at(1).at("code") == "output.limit_reached@1");
    CHECK(problems.at(1).at("fields").at("graph") == "other_render");
    CHECK(problems.at(1).at("fields").at("sink") == "shot");
    CHECK(problems.at(1).at("fields").at("limit") == "max_frames");
    CHECK(problems.at(1).at("fields").at("value") == 7);
    CHECK(problems.at(1).at("message").get<std::string>().find("max_frames") !=
          std::string::npos);

    // The two same-named sinks are in different graphs, so they are two
    // problems, and their ids differ.
    Pelican::ProblemList list;
    list.replace("output", problems);
    const auto stored = list.problemsOf("output");
    REQUIRE(stored.size() == 2);
    CHECK(stored.at(0).at("id") != stored.at(1).at("id"));

    // Rewriting the same states does not move the revision or change ids.
    const auto revision = list.revision();
    list.replace("output", Pelican::outputSinkProblems({refused, limited}));
    CHECK(list.revision() == revision);
    CHECK(list.problemsOf("output") == stored);

    // Running, inactive and finished-by-every_n sinks contribute no problem.
    auto running = refused;
    running.state = "running";
    running.last_error.clear();
    auto inactive = refused;
    inactive.state = "inactive";
    inactive.last_error.clear();
    auto finished = limited;
    finished.stop_reason = "none";
    finished.stop_value = 0;
    CHECK(Pelican::outputSinkProblems({running, inactive, finished}).empty());

    // Replacing the domain with an empty list withdraws the problems.
    const auto before_withdrawal = list.revision();
    list.replace("output", {});
    CHECK(list.revision() > before_withdrawal);
    CHECK(list.problemsOf("output").empty());
}

TEST_CASE("VO-1b a refused sink's code is the runtime's own code",
          "[vo1b][output-problem]") {
    using Pelican::OutputSinks;
    OutputSinks::State state;
    state.name = "shot";
    state.graph = "main_render";
    state.state = "refused";
    state.last_error = "output.format_unsupported@1: VK_FORMAT_R16G16B16A16_SFLOAT";
    const auto problems = Pelican::outputSinkProblems({state});
    REQUIRE(problems.size() == 1);
    CHECK(problems.at(0).at("code") == "output.format_unsupported@1");
    CHECK(problems.at(0).at("message") ==
          "output.format_unsupported@1: VK_FORMAT_R16G16B16A16_SFLOAT");
}
