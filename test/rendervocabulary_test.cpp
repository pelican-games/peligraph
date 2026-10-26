// RC-3a2: the attachment operations, the resolution domain, the gpu draw
// declaration and the frame-graph buffer declarations are the project's. This
// file includes project headers only and links pelican_project alone, so it
// compiles and links only while that holds.
#include "../src/project/attachmentoperations.hpp"
#include "../src/project/framegraphbufferdefinition.hpp"
#include "../src/project/gpudrawsource.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/graphparameterschema.hpp"
#include "../src/project/renderingjsonnormalize.hpp"
#include "../src/project/renderresolutiondomain.hpp"

#if defined(VULKAN_HPP) || defined(VULKAN_CORE_H_)
#error "the project's render vocabulary must not need Vulkan's headers"
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

namespace Pelican {

namespace {

using Json = nlohmann::json;

const GraphParameterSchema &parameter(std::string_view op, std::string_view name) {
    const auto *parameters = graphOperationParameters(op);
    REQUIRE(parameters != nullptr);
    const auto found = std::find_if(parameters->parameters.begin(), parameters->parameters.end(),
                                    [&](const auto &candidate) { return candidate.name == name; });
    REQUIRE(found != parameters->parameters.end());
    return *found;
}

// One pass whose `field` is `spelling`, in the rendering JSON's short form.
Json passWith(std::string_view field, std::string_view spelling) {
    return Json{{"rendering_passes",
                 Json::array({Json{{"name", "main"},
                                   {"passes", Json::array({Json{{"name", "p"},
                                                                {"type", "material"},
                                                                {std::string{field}, spelling},
                                                                {"output", {{"color", "c"}, {"depth", "d"}}}}})}}})}};
}

const Json &passOf(const Json &document) {
    return document.at("rendering_passes").at(0).at("passes").at(0);
}

} // namespace

TEST_CASE("RC-3a2 the project's render vocabulary needs neither core nor Vulkan",
          "[rc3a2][render-vocabulary]") {
    // The byte sizes are 64-bit, as Vulkan's VkDeviceSize; core checks the
    // indexed draw command's against Vulkan's struct.
    STATIC_REQUIRE(std::is_same_v<decltype(GpuDrawSourceDeclaration::command_offset), std::uint64_t>);
    STATIC_REQUIRE(std::is_same_v<decltype(GpuDrawSourceDeclaration::count_offset), std::uint64_t>);
    STATIC_REQUIRE(std::is_same_v<decltype(ComputeIndirectDispatchDefinition::offset), std::uint64_t>);
    STATIC_REQUIRE(std::is_same_v<decltype(FrameGraphBufferDefinition::size), std::uint64_t>);
    STATIC_REQUIRE(std::is_same_v<decltype(FrameGraphBufferExtentSizeDefinition::header_bytes), std::uint64_t>);
    STATIC_REQUIRE(std::is_same_v<decltype(FrameGraphBufferExtentSizeDefinition::bytes_per_tile), std::uint64_t>);
    STATIC_REQUIRE(frameGraphIndexedDrawCommandBytes == 20);
    STATIC_REQUIRE(frameGraphDrawCountBytes == 4);

    // A declaration over 4 GiB keeps its offsets.
    const GpuDrawSourceDeclaration declared{
        .commands = "draws",
        .count = "draw_count",
        .max_draw_count = 3,
        .command_offset = (std::uint64_t{1} << 33) + 4,
        .count_offset = std::uint64_t{1} << 33,
        .layout = GpuDrawSourceLayout::draw_queue_segments_v1,
        .execution = GpuDrawExecutionMode::cpu,
    };
    CHECK(declared.command_offset - declared.count_offset == 4);
    CHECK(declared.fallback == GpuDrawFallback::cpu_draw_queue);
    const FrameGraphBufferDefinition buffer{
        .name = "draws",
        .size = std::uint64_t{1} << 40,
        .command_layout = FrameGraphBufferCommandLayout::indexed_draw,
    };
    CHECK(buffer.size / frameGraphIndexedDrawCommandBytes > std::uint64_t{0xffffffffu});
    CHECK(std::string{frameGraphBufferCommandLayoutName(*buffer.command_layout)} == "indexed_draw");
    CHECK(std::string{frameGraphHostBufferSourceName(FrameGraphHostBufferSource::scene_draw_segments_v1)} ==
          "scene_draw_segments_v1");
    CHECK(ComputeIndirectDispatchDefinition{.buffer = "args", .offset = 12} ==
          ComputeIndirectDispatchDefinition{.buffer = "args", .offset = 12});
}

TEST_CASE("RC-3a2 a pass type's omitted attachment operations and resolution domain come from one place",
          "[rc3a2][attachment-operations][resolution-domain]") {
    STATIC_REQUIRE_FALSE(std::is_default_constructible_v<PassAttachmentOperations>);
    using L = AttachmentLoadOp;
    using S = AttachmentStoreOp;
    struct Row {
        RenderPassType type;
        PassAttachmentOperations color;
        RenderResolutionDomain domain;
    };
    const std::vector<Row> rows{
        {RenderPassType::material, PassAttachmentOperations{L::clear, S::store}, RenderResolutionDomain::scene},
        {RenderPassType::fullscreen, PassAttachmentOperations{L::clear, S::store},
         RenderResolutionDomain::unclassified},
        {RenderPassType::raster, PassAttachmentOperations{L::clear, S::store}, RenderResolutionDomain::unclassified},
        {RenderPassType::output_transform, PassAttachmentOperations{L::clear, S::store},
         RenderResolutionDomain::output},
        {RenderPassType::debug_draw, PassAttachmentOperations{L::clear, S::store},
         RenderResolutionDomain::unclassified},
        {RenderPassType::gizmo, PassAttachmentOperations{L::clear, S::store}, RenderResolutionDomain::output},
        {RenderPassType::debug_text, PassAttachmentOperations{L::clear, S::store},
         RenderResolutionDomain::unclassified},
        {RenderPassType::shadow_depth, PassAttachmentOperations{L::clear, S::store},
         RenderResolutionDomain::independent},
        {RenderPassType::velocity, PassAttachmentOperations{L::clear, S::store}, RenderResolutionDomain::scene},
        {RenderPassType::picking, PassAttachmentOperations{L::clear, S::store}, RenderResolutionDomain::scene},
        {RenderPassType::ui, PassAttachmentOperations{L::load, S::store}, RenderResolutionDomain::output},
        {RenderPassType::imgui, PassAttachmentOperations{L::load, S::store}, RenderResolutionDomain::output},
        {RenderPassType::canonical_anchor, PassAttachmentOperations{L::clear, S::store},
         RenderResolutionDomain::unclassified},
        {RenderPassType::snapshot_copy, PassAttachmentOperations{L::clear, S::store},
         RenderResolutionDomain::unclassified},
    };
    for (const auto &row : rows) {
        CAPTURE(std::string{renderPassTypeName(row.type)});
        CHECK(defaultPassAttachmentOperations(row.type, AttachmentAspect::color) == row.color);
        CHECK(defaultPassAttachmentOperations(row.type, AttachmentAspect::depth) ==
              PassAttachmentOperations{L::clear, S::dont_care});
        CHECK(defaultRenderResolutionDomain(row.type) == row.domain);
        CHECK(parseRenderResolutionDomain(Json::object(), row.type, "p") == row.domain);
    }
    // The order: the attachment's own, then the pass's, then the defaults.
    const PassAttachmentOperations defaults{L::clear, S::store};
    CHECK(resolveAttachmentOperations(L::load, std::nullopt, L::dont_care, S::dont_care, defaults) ==
          PassAttachmentOperations{L::load, S::dont_care});
    CHECK(resolveAttachmentOperations(std::nullopt, std::nullopt, L::dont_care, std::nullopt, defaults) ==
          PassAttachmentOperations{L::dont_care, S::store});
    CHECK(resolveAttachmentOperations(std::nullopt, std::nullopt, std::nullopt, std::nullopt, defaults) ==
          defaults);

    CHECK(parseRenderResolutionDomain(Json{{"resolution_domain", "none"}}, RenderPassType::material, "p") ==
          RenderResolutionDomain::unclassified);
    CHECK_THROWS_WITH(parseRenderResolutionDomain(Json{{"resolution_domain", "world"}}, RenderPassType::material, "p"),
                      "Unknown pass resolution_domain 'world': p");
    CHECK_THROWS_WITH(parseRenderResolutionDomain(Json{{"resolution_domain", 1}}, RenderPassType::material, "p"),
                      "Pass resolution_domain must be a string: p");

    // Studio's schema shows each pass op's own default domain.
    for (const auto &[op, type] : std::vector<std::pair<std::string_view, RenderPassType>>{
             {"render.material_pass", RenderPassType::material},
             {"render.fullscreen", RenderPassType::fullscreen},
             {"render.raster", RenderPassType::raster},
             {"render.shadow_depth", RenderPassType::shadow_depth}}) {
        CAPTURE(std::string{op});
        const auto &domain = parameter(op, "resolution_domain");
        REQUIRE(domain.default_value.has_value());
        CHECK(domain.default_value->get<std::string>() == std::string{renderResolutionDomainName(defaultRenderResolutionDomain(type))});
        CHECK(domain.enum_values ==
              std::vector<std::string>{"scene", "output", "independent", "unclassified", "none"});
    }
}

TEST_CASE("RC-3a2 each way in admits the attachment op spellings it did and refuses the rest by name",
          "[rc3a2][attachment-operations][spelling]") {
    using L = AttachmentLoadOp;
    using S = AttachmentStoreOp;
    const std::vector<std::pair<std::string, L>> authored_loads{
        {"clear", L::clear}, {"Clear", L::clear},         {"load", L::load},          {"Load", L::load},
        {"dont_care", L::dont_care}, {"DontCare", L::dont_care}, {"dontCare", L::dont_care}};
    const std::vector<std::pair<std::string, S>> authored_stores{
        {"store", S::store}, {"Store", S::store}, {"dont_care", S::dont_care}, {"DontCare", S::dont_care},
        {"dontCare", S::dont_care}};

    SECTION("authored: the pass parser's and Studio's") {
        for (const auto &[spelling, op] : authored_loads) {
            CAPTURE(spelling);
            CHECK(findAuthoredAttachmentLoadOp(spelling) == op);
        }
        for (const auto &[spelling, op] : authored_stores) {
            CAPTURE(spelling);
            CHECK(findAuthoredAttachmentStoreOp(spelling) == op);
        }
        for (const std::string spelling : {"discard", "Discard", "LOAD", "dont-care", ""}) {
            CAPTURE(spelling);
            CHECK_FALSE(findAuthoredAttachmentLoadOp(spelling).has_value());
        }
        for (const std::string spelling : {"discard", "load", "clear", ""}) {
            CAPTURE(spelling);
            CHECK_FALSE(findAuthoredAttachmentStoreOp(spelling).has_value());
        }
        // Studio offers and checks exactly these.
        const std::vector<std::string> load_values{"clear", "load", "dont_care", "Clear", "Load", "DontCare",
                                                   "dontCare"};
        const std::vector<std::string> store_values{"store", "dont_care", "Store", "DontCare", "dontCare"};
        for (const auto field : {"color_load_op", "depth_load_op"})
            CHECK(parameter("render.material_pass", field).enum_values == load_values);
        for (const auto field : {"color_store_op", "depth_store_op"})
            CHECK(parameter("render.material_pass", field).enum_values == store_values);
        CHECK(parameter("render.shadow_depth", "depth_load_op").enum_values == load_values);
        try {
            checkGraphNodeParameterTypes(
                "render.material_pass",
                Json{{"op", "render.material_pass"}, {"id", "p"}, {"parameters", {{"color_load_op", "discard"}}}});
            FAIL("Studio's schema took color_load_op \"discard\"");
        } catch (const GraphRegistryError &error) {
            CHECK(error.code() == "graph.render_node_parameter_invalid@1");
            CHECK(error.detail().at("parameter") == "color_load_op");
            CHECK(error.detail().at("reason") == "enum");
        }
    }

    SECTION("normalized: one canonical spelling each; an unknown one is left for the pass parser") {
        for (const auto &[spelling, op] : authored_loads) {
            CAPTURE(spelling);
            const auto normalized = normalizeRenderingJson(passWith("color_load_op", spelling));
            CHECK(passOf(normalized).at("color_load_op").get<std::string>() == std::string{attachmentLoadOpName(op)});
            REQUIRE_NOTHROW(requireNormalizedRenderingJson(normalized));
            if (spelling != attachmentLoadOpName(op)) {
                CHECK_THROWS_AS(requireNormalizedRenderingJson(passWith("color_load_op", spelling)),
                                GraphRegistryError);
            }
        }
        for (const auto &[spelling, op] : authored_stores) {
            CAPTURE(spelling);
            CHECK(passOf(normalizeRenderingJson(passWith("depth_store_op", spelling))).at("depth_store_op").get<std::string>() ==
                  std::string{attachmentStoreOpName(op)});
        }
        CHECK(std::string{attachmentLoadOpName(L::dont_care)} == "dont_care");
        CHECK(std::string{attachmentStoreOpName(S::dont_care)} == "dont_care");
        CHECK(passOf(normalizeRenderingJson(passWith("color_load_op", "discard"))).at("color_load_op") ==
              "discard");
    }

    SECTION("physical plan: its own three and two, written as it reads them") {
        CHECK(findPlanAttachmentLoadOp("clear") == L::clear);
        CHECK(findPlanAttachmentLoadOp("load") == L::load);
        CHECK(findPlanAttachmentLoadOp("discard") == L::dont_care);
        CHECK(findPlanAttachmentStoreOp("store") == S::store);
        CHECK(findPlanAttachmentStoreOp("discard") == S::dont_care);
        for (const std::string spelling : {"dont_care", "DontCare", "Clear", "Load", "dontCare", ""}) {
            CAPTURE(spelling);
            CHECK_FALSE(findPlanAttachmentLoadOp(spelling));
            CHECK_FALSE(findPlanAttachmentStoreOp(spelling));
        }
        CHECK(std::string{attachmentLoadOpPlanName(L::dont_care)} == "discard");
        CHECK(std::string{attachmentStoreOpPlanName(S::dont_care)} == "discard");
        CHECK(std::string{attachmentLoadOpPlanName(L::clear)} == "clear");
        CHECK(std::string{attachmentStoreOpPlanName(S::store)} == "store");
        CHECK(std::string{attachmentAspectName(AttachmentAspect::depth)} == "depth");
    }
}

} // namespace Pelican
