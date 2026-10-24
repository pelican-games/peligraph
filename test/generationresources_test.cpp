// RC-3c2a: the generation's resource description (resolveGenerationResources)
// over parsed IRs and hand-written placements: the merge rules with their
// concrete values, the alias groups that stand or leave, the buffers' sizes
// and agreement, the engine's usage rule, and every refusal by code and
// subject. The production registration's use of it is
// generationresources_device_test.cpp.
#include "../src/project/generationresources.hpp"
#include "../src/project/graphcomposition.hpp"
#include "../src/project/rendergraphparse.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <string>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;

Json image(const std::string &name, Json extra = Json::object()) {
    Json result{{"name", name},
                {"extent_scale", 1.0},
                {"format", "R8G8B8A8_UNORM"},
                {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
    result.update(extra);
    return result;
}

RenderGraphIR irOf(Json images, Json buffers = Json::array()) {
    Json config{{"render_targets", std::move(images)}};
    if (!buffers.empty()) config["buffers"] = std::move(buffers);
    return parseRenderGraph(config, PassFieldOwnershipCapabilities{});
}

ImagePlacement placed(const std::string &resource, std::uint32_t layers = 1,
                      RenderTargetStorageMode storage = RenderTargetStorageMode::materialized,
                      std::optional<std::string> alias_group = std::nullopt) {
    return ImagePlacement{.resource = resource,
                          .array_layers = layers,
                          .storage = storage,
                          .alias_group = std::move(alias_group)};
}

// The named refusal of `call`: "<code> <detail>".
template <class Call>
std::string refusal(Call &&call) {
    try {
        (void)call();
    } catch (const GraphRegistryError &error) {
        return error.what();
    }
    return "accepted";
}

const GenerationImage &imageOf(const GenerationResources &resources, std::string_view name) {
    const auto *found = resources.findImage(name);
    REQUIRE(found != nullptr);
    return *found;
}

} // namespace

TEST_CASE("RC-3c2a the engine's usage: one rule for the plan's question and the merged image",
          "[generation-resources][rc3c2a][usage]") {
    const ImageUsage color = ImageUsageBit::color_attachment | ImageUsageBit::sampled;
    const ImageUsage depth = ImageUsageBit::depth_stencil_attachment;
    CHECK(engineImageUsage({.declared = color}) == color);
    CHECK(engineImageUsage({.declared = depth, .storage = RenderTargetStorageMode::transient_attachment}) ==
          (depth | ImageUsageBit::transient_attachment));
    // A tile-local attachment keeps its attachment bits only.
    CHECK(engineImageUsage({.declared = color, .storage = RenderTargetStorageMode::tile_local_attachment}) ==
          (ImageUsageBit::color_attachment | ImageUsageBit::input_attachment |
           ImageUsageBit::transient_attachment));
    CHECK(engineImageUsage({.declared = color, .history = true}) == (color | ImageUsageBit::transfer_dst));
    CHECK(engineImageUsage({.declared = color, .local_read = true}) == (color | ImageUsageBit::input_attachment));
    CHECK(engineImageUsage({.declared = depth, .external_depth_source = true}) ==
          (depth | ImageUsageBit::transfer_src));
    // A final usage is a fixed point (a definition made from the
    // description registers as it is).
    for (const auto storage : {RenderTargetStorageMode::materialized, RenderTargetStorageMode::transient_attachment,
                               RenderTargetStorageMode::tile_local_attachment}) {
        const ImageUsageFacts facts{.declared = color, .history = false, .storage = storage, .local_read = true};
        const auto final_usage = engineImageUsage(facts);
        CHECK(engineImageUsage({.declared = final_usage, .storage = storage}) == final_usage);
    }
    // The uses a graph image's declaration is completed with.
    CHECK(requiredImageUsage(ImageUseRequirement::copy_source) == ImageUsage{ImageUsageBit::transfer_src});
    CHECK(requiredImageUsage(ImageUseRequirement::copy_destination) ==
          (ImageUsageBit::transfer_dst | ImageUsageBit::sampled));
    CHECK(requiredImageUsage(ImageUseRequirement::file_source_mips) ==
          (ImageUsageBit::sampled | ImageUsageBit::transfer_dst | ImageUsageBit::transfer_src));
}

TEST_CASE("RC-3c2a the generation merges its variants' images: usage, layers, storage, alias groups",
          "[generation-resources][rc3c2a][merge]") {
    // flat: depth transient, scene single-layer, three alias groups placed.
    // xr: depth materialized and exported, scene two layers and sampled by
    // a reader flat has not, the healthy group placed again, the partial
    // group's b1 alone (b2 is not in xr), the absent group's members not in
    // xr at all.
    const auto depth = image("depth", {{"format", "D32_SFLOAT"},
                                       {"usage", Json::array({"DEPTH_STENCIL_ATTACHMENT"})}});
    const auto flat_ir = irOf(Json::array({depth, image("scene", {{"usage", Json::array({"COLOR_ATTACHMENT"})}}),
                                           image("a1"), image("a2"), image("b1"), image("b2"), image("c1"),
                                           image("c2")}));
    const auto xr_ir = irOf(Json::array({depth, image("scene"), image("a1"), image("a2"), image("b1")}));
    const auto group_a = std::optional<std::string>{"runtime-alias/a1/a2"};
    const auto group_b = std::optional<std::string>{"runtime-alias/b1/b2"};
    const auto group_c = std::optional<std::string>{"runtime-alias/c1/c2"};
    std::vector<GenerationResourceVariant> variants{
        {.name = "flat",
         .render_graph = &flat_ir,
         .placements = {placed("depth", 1, RenderTargetStorageMode::transient_attachment), placed("scene"),
                        placed("a1", 1, RenderTargetStorageMode::materialized, group_a),
                        placed("a2", 1, RenderTargetStorageMode::materialized, group_a),
                        placed("b1", 1, RenderTargetStorageMode::materialized, group_b),
                        placed("b2", 1, RenderTargetStorageMode::materialized, group_b),
                        placed("c1", 1, RenderTargetStorageMode::materialized, group_c),
                        placed("c2", 1, RenderTargetStorageMode::materialized, group_c)}},
        {.name = "xr",
         .render_graph = &xr_ir,
         .placements = {placed("depth", 2), placed("scene", 2),
                        placed("a1", 2, RenderTargetStorageMode::materialized, group_a),
                        placed("a2", 2, RenderTargetStorageMode::materialized, group_a), placed("b1", 2)}},
    };
    variants[1].placements[0].external_depth_source = true;
    const auto resources = resolveGenerationResources(variants, {1920, 1080});

    const auto &merged_depth = imageOf(resources, "depth");
    // Materialized where the variants differ, its usage computed after that:
    // the export's TRANSFER_SRC, no transient bit.
    CHECK(merged_depth.storage == RenderTargetStorageMode::materialized);
    CHECK(merged_depth.usage == (ImageUsageBit::depth_stencil_attachment | ImageUsageBit::transfer_src));
    CHECK(merged_depth.array_layers == 2);
    const auto &scene = imageOf(resources, "scene");
    CHECK(scene.usage == (ImageUsageBit::color_attachment | ImageUsageBit::sampled));
    CHECK(scene.array_layers == 2);
    CHECK(scene.extent == ResolvedResourceExtent{1920, 1080});
    // The groups: a stands (both variants place both), b leaves (xr has b1
    // without b2), c stands (xr has neither).
    CHECK(imageOf(resources, "a1").alias_group == group_a);
    CHECK(imageOf(resources, "a2").alias_group == group_a);
    CHECK_FALSE(imageOf(resources, "b1").alias_group);
    CHECK_FALSE(imageOf(resources, "b2").alias_group);
    CHECK(imageOf(resources, "c1").alias_group == group_c);
    CHECK(imageOf(resources, "c2").alias_group == group_c);
    // Each variant registers its own images, in its order.
    REQUIRE(resources.variants.size() == 2);
    CHECK(resources.variants[0].images.size() == 8);
    REQUIRE(resources.variants[1].images.size() == 5);
    CHECK(resources.images[resources.variants[1].images[4]].declaration.name == "b1");

    // A variant that places a member of a group outside it breaks the group
    // too (xr has a1 and a2 but aliases neither).
    auto unplaced = variants;
    unplaced[1].placements[2].alias_group.reset();
    unplaced[1].placements[3].alias_group.reset();
    const auto broken = resolveGenerationResources(unplaced, {1920, 1080});
    CHECK_FALSE(imageOf(broken, "a1").alias_group);
    CHECK_FALSE(imageOf(broken, "a2").alias_group);
    CHECK(imageOf(broken, "c1").alias_group == group_c);
}

TEST_CASE("RC-3c2a the generation refuses by name what its variants disagree on",
          "[generation-resources][rc3c2a][refusal]") {
    const auto two = [](Json flat_image, Json xr_image, ImagePlacement flat_place = placed("t"),
                        ImagePlacement xr_place = placed("t")) {
        const auto flat_ir = irOf(Json::array({std::move(flat_image)}));
        const auto xr_ir = irOf(Json::array({std::move(xr_image)}));
        const std::array variants{
            GenerationResourceVariant{.name = "flat", .render_graph = &flat_ir, .placements = {flat_place}},
            GenerationResourceVariant{.name = "xr", .render_graph = &xr_ir, .placements = {xr_place}}};
        return refusal([&] { return resolveGenerationResources(variants, {1920, 1080}); });
    };
    const auto conflict = [](std::string_view field) {
        return std::string{R"(render_graph.image_variant_conflict@1 {"field":")"} + std::string{field} +
               R"(","resource":"t","variants":["flat","xr"]})";
    };
    CHECK(two(image("t", {{"mip_levels", 2}}), image("t")) == conflict("mip_levels"));
    CHECK(two(image("t", {{"dimension", "cube"}, {"width", 8}, {"height", 8}}),
              image("t", {{"width", 8}, {"height", 8}})) == conflict("dimension"));
    CHECK(two(image("t", {{"extent_scale", 0.5}}), image("t")) == conflict("extent"));
    CHECK(two(image("t", {{"history", true}}), image("t")) == conflict("history"));
    auto other_format = placed("t");
    other_format.format = ImageFormat::r16g16b16a16_sfloat;
    CHECK(two(image("t"), image("t"), placed("t"), other_format) == conflict("format"));
    auto multisampled = placed("t");
    multisampled.samples = 4;
    CHECK(two(image("t"), image("t"), placed("t"), multisampled) == conflict("samples"));
    CHECK(two(image("t"), image("t")) == "accepted");
    // A file image read from another file in one variant.
    const auto file = [](const std::string &asset) {
        return image("t", {{"width", 4}, {"height", 4}, {"usage", Json::array({"SAMPLED", "TRANSFER_DST"})},
                           {"source", {{"asset", asset}}}});
    };
    CHECK(two(file("project://a.png"), file("project://b.png")) == conflict("source"));
    CHECK(two(file("project://a.png"), file("project://a.png")) == "accepted");

    // Placements: of no config image, twice, or missing.
    const auto ir = irOf(Json::array({image("t"), image("u")}));
    const auto placing = [&](std::vector<ImagePlacement> placements) {
        const std::array variants{
            GenerationResourceVariant{.name = "flat", .render_graph = &ir, .placements = std::move(placements)}};
        return refusal([&] { return resolveGenerationResources(variants, {64, 64}); });
    };
    CHECK(placing({placed("t"), placed("u"), placed("v")}) ==
          R"(render_graph.image_placement_invalid@1 {"reason":"unknown_image","resource":"v","variant":"flat"})");
    CHECK(placing({placed("t"), placed("t")}) ==
          R"(render_graph.image_placement_invalid@1 {"reason":"duplicate","resource":"t","variant":"flat"})");
    CHECK(placing({placed("t")}) ==
          R"(render_graph.image_placement_invalid@1 {"reason":"missing","resource":"u","variant":"flat"})");
    // None at all is missing too (an image as declared is the editor's own
    // entry, resolveDeclaredGenerationResources).
    CHECK(placing({}) ==
          R"(render_graph.image_placement_invalid@1 {"reason":"missing","resource":"t","variant":"flat"})");

    // A group left with one member, or of two contracts.
    const auto group = std::optional<std::string>{"g"};
    CHECK(placing({placed("t", 1, RenderTargetStorageMode::materialized, group), placed("u")}) ==
          R"(render_graph.alias_group_invalid@1 {"group":"g","reason":"member_count"})");
    CHECK(placing({placed("t", 1, RenderTargetStorageMode::materialized, group),
                   placed("u", 2, RenderTargetStorageMode::materialized, group)}) ==
          R"(render_graph.alias_group_invalid@1 {"group":"g","reason":"contract","resource":"u"})");
    CHECK(placing({placed("t", 1, RenderTargetStorageMode::materialized, group),
                   placed("u", 1, RenderTargetStorageMode::materialized, group)}) == "accepted");
}

TEST_CASE("RC-3c2a an image whose layers the runtime decides is never aliased, history, a cube or a file",
          "[generation-resources][rc3c2a][runtime-layers]") {
    const auto resolve = [](Json shadow, bool grouped) {
        const auto ir = irOf(Json::array({std::move(shadow), image("other")}));
        const auto group = grouped ? std::optional<std::string>{"g"} : std::nullopt;
        const std::array variants{GenerationResourceVariant{
            .name = "flat",
            .render_graph = &ir,
            .placements = {placed("shadow", 1, RenderTargetStorageMode::materialized, group),
                           placed("other", 1, RenderTargetStorageMode::materialized, group)},
            .runtime_layered_images = {"shadow"}}};
        return resolveGenerationResources(variants, {64, 64});
    };
    // A group with it leaves (both members), the static constraint marked.
    const auto resources = resolve(image("shadow"), true);
    CHECK(imageOf(resources, "shadow").runtime_array_layers);
    CHECK_FALSE(imageOf(resources, "shadow").alias_group);
    CHECK_FALSE(imageOf(resources, "other").alias_group);
    CHECK_FALSE(imageOf(resources, "other").runtime_array_layers);
    CHECK(refusal([&] { return resolve(image("shadow", {{"history", true}}), false); }) ==
          R"(render_graph.image_runtime_layers_invalid@1 {"reason":"history","resource":"shadow"})");
    CHECK(refusal([&] {
              return resolve(image("shadow", {{"dimension", "cube"}, {"width", 8}, {"height", 8}}), false);
          }) == R"(render_graph.image_runtime_layers_invalid@1 {"reason":"not_2d","resource":"shadow"})");
}

TEST_CASE("RC-3c2a buffers: one declaration per name, sized from their image's resolved extent",
          "[generation-resources][rc3c2a][buffers]") {
    const auto images = Json::array({image("tiny", {{"width", 64}, {"height", 64}}),
                                     image("lit_color"), image("third", {{"extent_scale", 0.3333333333}})});
    const Json selection{{"name", "selection"},
                         {"size_from_extent",
                          {{"resource", "lit_color"}, {"tile_width", 32}, {"tile_height", 32},
                           {"header_bytes", 48}, {"bytes_per_tile", 260}, {"copies", 2}}}};
    const auto ir = irOf(images, Json::array({selection}));
    // (ii) lit_color at 1280x720 (40x23 tiles), not the first fixed image
    // (tiny, 2x2 tiles: the old rule's 2,176 bytes).
    const auto sized = resolveDeclaredGenerationResources(ir, {1280, 720});
    REQUIRE(sized.buffers.size() == 1);
    CHECK(sized.buffers.front().size == 478'496);
    CHECK(ir.buffers.front().size == 0);
    // The extent rule is the images' (float scale, truncated): a third of
    // 1920x1080 is 640x360, and (iii) a 1x1-tile buffer of 4 bytes per
    // texel sized from it holds 640x360x4 = 921,600 bytes (the old double
    // rule's 639x359 made 917,604).
    const Json texels{{"name", "texels"},
                      {"size_from_extent",
                       {{"resource", "third"}, {"tile_width", 1}, {"tile_height", 1},
                        {"header_bytes", 0}, {"bytes_per_tile", 4}}}};
    const auto texel_ir = irOf(images, Json::array({texels}));
    const auto at_1080 = resolveDeclaredGenerationResources(texel_ir, {1920, 1080});
    CHECK(imageOf(at_1080, "third").extent == ResolvedResourceExtent{640, 360});
    CHECK(at_1080.buffers.front().size == 921'600);
    // A size from an extent is held to the buffer's contract once sized.
    auto odd = selection;
    odd["index_buffer"] = true;
    odd["size_from_extent"]["header_bytes"] = 1;
    const auto odd_ir = irOf(images, Json::array({odd}));
    CHECK(refusal([&] { return resolveDeclaredGenerationResources(odd_ir, {1280, 720}); }) ==
          R"(render_graph.buffer_invalid@1 {"field":"size","reason":"index_buffer_not_uint32_aligned","resource":"selection"})");
    // The image a size comes from is a config image.
    auto unknown = selection;
    unknown["size_from_extent"]["resource"] = "nowhere";
    CHECK(refusal([&] { return irOf(images, Json::array({unknown})); }) ==
          R"(render_graph.buffer_invalid@1 {"field":"size_from_extent","reason":"unknown_image","resource":"selection"})");
}

TEST_CASE("RC-3c2a initial contents: omitted is zero, an explicit empty list is refused",
          "[generation-resources][rc3c2a][buffers]") {
    const auto with = [](Json extra) {
        Json buffer{{"name", "b"}, {"size", 4}, {"host_source", "runtime_f32"}};
        buffer.update(extra);
        return refusal([&] {
            const auto ir = irOf(Json::array(), Json::array({buffer}));
            return resolveDeclaredGenerationResources(ir, {64, 64}).buffers.at(0).initial_f32;
        });
    };
    CHECK(with(Json::object()) == "accepted");
    CHECK(with({{"initial_f32", Json::array({0.5})}}) == "accepted");
    CHECK(with({{"initial_f32", Json::array()}}) ==
          R"(render_graph.buffer_invalid@1 {"field":"initial_f32","reason":"empty","resource":"b"})");
    const auto ir = irOf(Json::array(), Json::array({Json{{"name", "b"}, {"size", 4}, {"host_source", "runtime_f32"}}}));
    CHECK(resolveDeclaredGenerationResources(ir, {64, 64}).buffers.at(0).initial_f32.empty());
}

namespace {

// The bits of a buffer's initial contents, as the GPU receives them.
std::vector<std::uint32_t> bitsOf(const std::vector<float> &values) {
    std::vector<std::uint32_t> bits;
    for (const auto value : values) bits.push_back(std::bit_cast<std::uint32_t>(value));
    return bits;
}

} // namespace

TEST_CASE("RC-3c2a buffers of one name agree in every field, whichever variant comes first",
          "[generation-resources][rc3c2a][buffers]") {
    const auto images = Json::array({image("lit_color")});
    // Two variants declaring `left` and `right` as the buffer "data".
    const auto resolve = [&](Json left, Json right, bool reversed) {
        left["name"] = "data";
        right["name"] = "data";
        const auto left_ir = irOf(images, Json::array({std::move(left)}));
        const auto right_ir = irOf(images, Json::array({std::move(right)}));
        std::vector<GenerationResourceVariant> variants{
            {.name = "flat", .render_graph = &left_ir, .placements = {placed("lit_color")}},
            {.name = "xr", .render_graph = &right_ir, .placements = {placed("lit_color")}}};
        if (reversed) std::swap(variants[0], variants[1]);
        return resolveGenerationResources(variants, {64, 64});
    };
    for (const bool reversed : {false, true}) {
        INFO("reversed " << reversed);
        const std::string first = reversed ? "xr" : "flat";
        const std::string second = reversed ? "flat" : "xr";
        // Each pair differs in exactly the named field.
        const auto refused = [&](Json left, Json right, std::string_view field) {
            INFO(field);
            CHECK(refusal([&] { return resolve(left, right, reversed); }) ==
                  std::string{R"(render_graph.buffer_variant_conflict@1 {"field":")"} + std::string{field} +
                      R"(","resource":"data","variants":[")" + first + R"(",")" + second + R"("]})");
        };
        const Json data{{"size", 16}};
        refused(data, {{"size", 32}}, "size");
        refused(data, {{"size", 16}, {"lifetime", "transient"}}, "lifetime");
        refused(data, {{"size", 16}, {"index_buffer", true}}, "index_buffer");
        refused(data, {{"size", 16}, {"command_layout", "compute_dispatch"}}, "command_layout");
        refused({{"size", 16}, {"host_source", "scene_lights_v2"}}, {{"size", 16}, {"host_source", "runtime_f32"}},
                "host_source");
        refused({{"size", 64}, {"host_source", "view_family_matrices_v1"}, {"view_family", "$project/a"}},
                {{"size", 64}, {"host_source", "view_family_matrices_v1"}, {"view_family", "$project/b"}},
                "view_family");
        const auto tiled = [](std::uint32_t tile) {
            return Json{{"size_from_extent",
                         {{"resource", "lit_color"}, {"tile_width", tile}, {"tile_height", 8},
                          {"header_bytes", 0}, {"bytes_per_tile", 4}}}};
        };
        refused(tiled(8), tiled(16), "size_from_extent");
        const auto contents = [](float value) {
            return Json{{"size", 4}, {"host_source", "runtime_f32"}, {"initial_f32", Json::array({value})}};
        };
        refused(contents(0.5f), contents(0.25f), "initial_f32");
        // +0.0 and -0.0 are equal floats and different contents.
        refused(contents(0.0f), contents(-0.0f), "initial_f32");
        // Agreeing, the resolved contents' bits are the same either way.
        const auto agreed = resolve(contents(-0.0f), contents(-0.0f), reversed);
        REQUIRE(agreed.buffers.size() == 1);
        CHECK(bitsOf(agreed.buffers.front().initial_f32) == std::vector<std::uint32_t>{0x80000000u});
        CHECK(agreed.buffers.front().host_source == FrameGraphHostBufferSource::runtime_f32);
        CHECK(agreed.variants[0].buffers == std::vector<std::size_t>{0});
        CHECK(agreed.variants[1].buffers == std::vector<std::size_t>{0});
    }
}

TEST_CASE("RC-3c2a a use is an image or a buffer as its own variant declares it",
          "[generation-resources][rc3c2a][checks]") {
    // flat reads its buffer `b` in a fullscreen pass; xr declares an image
    // `b` (a color attachment only, never sampled).
    const auto with_pass = [](Json targets, Json buffers) {
        Json config{{"render_targets", std::move(targets)},
                    {"rendering_passes",
                     Json::array({Json{{"name", "main"},
                                       {"passes",
                                        Json::array({Json{{"name", "read_b"},
                                                          {"type", "fullscreen"},
                                                          {"input", Json::array({"b"})},
                                                          {"output", {{"color", Json::array({Json{{"target", "out"}}})},
                                                                      {"depth", nullptr}}}}})}}})}};
        if (!buffers.empty()) config["buffers"] = std::move(buffers);
        return parseRenderGraph(config, PassFieldOwnershipCapabilities{});
    };
    const auto color_only = image("b", {{"usage", Json::array({"COLOR_ATTACHMENT"})}});
    const auto flat_ir = with_pass(Json::array({image("out")}), Json::array({Json{{"name", "b"}, {"size", 16}}}));
    const auto xr_ir = irOf(Json::array({image("out"), color_only}));
    const std::array variants{
        GenerationResourceVariant{.name = "flat", .render_graph = &flat_ir, .placements = {placed("out")}},
        GenerationResourceVariant{.name = "xr", .render_graph = &xr_ir, .placements = {placed("out"), placed("b")}}};
    const auto resources = resolveGenerationResources(variants, {64, 64});
    REQUIRE(resources.findImage("b") != nullptr);
    // flat's `b` is its buffer: no SAMPLED asked of xr's image.
    std::vector<std::string> read;
    CHECK(refusal([&] {
              validateRenderGraphResources(flat_ir, resources, [&](const ResourceCheckRead &image) {
                  read.push_back(image.image.declaration.name);
              });
              return 0;
          }) == "accepted");
    CHECK(std::find(read.begin(), read.end(), "b") == read.end());
    CHECK(std::find(read.begin(), read.end(), "out") != read.end());
    // Within one variant an image `b` is still checked.
    const auto image_ir = with_pass(Json::array({image("out"), color_only}), Json::array());
    const GenerationResourceVariant alone{.name = "flat", .render_graph = &image_ir,
                                          .placements = {placed("out"), placed("b")}};
    CHECK(refusal([&] {
              validateRenderGraphResources(image_ir, resolveGenerationResources(std::span{&alone, 1}, {64, 64}));
              return 0;
          }) ==
          R"(render_graph.image_usage_missing@1 {"graph":"main","node":"read_b","resource":"b","usage":"SAMPLED"})");
}

TEST_CASE("RC-3c2a a snapshot copy is sized from its destination as the generation resolves it",
          "[generation-resources][rc3c2a][snapshot]") {
    // One base, the output's: (i) a third of 1920x1080 is 640x360 by the
    // float rule the image is made with (the parse's own double arithmetic
    // made 639x359); and a copy sized from a small fixed display image
    // before is now the image's own size at the output (16,384 -> 3,686,400
    // bytes at 1280x720).
    const auto config = [](Json display_size) {
        Json display{{"name", "display"}, {"format_class", "display"}, {"extent_scale", 1.0},
                     {"format", "B8G8R8A8_SRGB"}, {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED"})}};
        display.update(display_size);
        return Json{
            {"render_targets",
             Json::array({display,
                          image("src", {{"extent_scale", 0.3333333333},
                                        {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"})}}),
                          image("copy", {{"extent_scale", 0.3333333333},
                                         {"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_DST"})}}),
                          image("full_src", {{"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"})}}),
                          image("full", {{"usage", Json::array({"COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_DST"})}})})},
            {"rendering_passes",
             Json::array({Json{{"name", "main"},
                               {"passes",
                                Json::array({Json{{"name", "fill"}, {"type", "fullscreen"},
                                                  {"output", {{"color", Json::array({Json{{"target", "src"}}})},
                                                              {"depth", nullptr}}}},
                                             Json{{"name", "snap"}, {"type", "snapshot_copy"}, {"source", "src"},
                                                  {"destination", "copy"}, {"snapshot_after", "fill"}},
                                             Json{{"name", "fill_full"}, {"type", "fullscreen"},
                                                  {"output", {{"color", Json::array({Json{{"target", "full_src"}}})},
                                                              {"depth", nullptr}}}},
                                             Json{{"name", "snap_full"}, {"type", "snapshot_copy"},
                                                  {"source", "full_src"}, {"destination", "full"},
                                                  {"snapshot_after", "fill_full"}}})}}})}};
    };
    const auto finalized = [](const Json &document, ResolvedResourceExtent output) {
        TargetPlanningPolicy planning;
        return finalizeRenderGraph(parseRenderGraph(document, PassFieldOwnershipCapabilities{}),
                                   FinalizeRenderGraphRequest{.output_extent = output}, planning);
    };
    const auto bytes = [](const RenderGraphIR &ir, std::size_t node) {
        return std::get<SnapshotNodeInfo>(ir.graphs.at(0).nodes.at(node).info).byte_size;
    };
    const auto at_1080 = finalized(config({{"width", 1920}, {"height", 1080}}), {1920, 1080});
    CHECK(bytes(at_1080, 1) == std::size_t{640} * 360 * 4);
    CHECK(imageOf(resolveDeclaredGenerationResources(at_1080, {1920, 1080}), "copy").extent ==
          ResolvedResourceExtent{640, 360});
    const auto small_display = finalized(config({{"width", 64}, {"height", 64}}), {1280, 720});
    CHECK(bytes(small_display, 3) == std::size_t{1280} * 720 * 4);
    CHECK(imageOf(resolveDeclaredGenerationResources(small_display, {1280, 720}), "full").extent ==
          ResolvedResourceExtent{1280, 720});
    // A revision before finalize holds no size.
    CHECK(bytes(parseRenderGraph(config({{"width", 64}, {"height", 64}}), PassFieldOwnershipCapabilities{}), 3) == 0);
}

} // namespace Pelican
