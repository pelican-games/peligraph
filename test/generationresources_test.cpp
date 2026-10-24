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

#include <array>
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
    const std::array one{GenerationResourceVariant{.name = "flat", .render_graph = &ir}};
    // (ii) lit_color at 1280x720 (40x23 tiles), not the first fixed image
    // (tiny, 2x2 tiles: the old rule's 2,176 bytes).
    const auto sized = resolveGenerationResources(one, {1280, 720});
    REQUIRE(sized.buffers.size() == 1);
    CHECK(sized.buffers.front().size == 478'496);
    CHECK(ir.buffers.front().size == 0);
    // The extent rule is the images' (float scale, truncated): a third of
    // 1920x1080 is 640x360.
    CHECK(imageOf(resolveGenerationResources(one, {1920, 1080}), "third").extent ==
          ResolvedResourceExtent{640, 360});
    // A size from an extent is held to the buffer's contract once sized.
    auto odd = selection;
    odd["index_buffer"] = true;
    odd["size_from_extent"]["header_bytes"] = 1;
    const auto odd_ir = irOf(images, Json::array({odd}));
    const std::array odd_variant{GenerationResourceVariant{.name = "flat", .render_graph = &odd_ir}};
    CHECK(refusal([&] { return resolveGenerationResources(odd_variant, {1280, 720}); }) ==
          R"(render_graph.buffer_invalid@1 {"field":"size","reason":"index_buffer_not_uint32_aligned","resource":"selection"})");
    // The image a size comes from is a config image.
    auto unknown = selection;
    unknown["size_from_extent"]["resource"] = "nowhere";
    CHECK(refusal([&] { return irOf(images, Json::array({unknown})); }) ==
          R"(render_graph.buffer_invalid@1 {"field":"size_from_extent","reason":"unknown_image","resource":"selection"})");

    // Every field of one name agrees across the variants, whichever comes
    // first; otherwise the first field that differs is named.
    const Json data{{"name", "data"}, {"size", 16}, {"host_source", "runtime_f32"},
                    {"initial_f32", Json::array({1.0, 2.0, 3.0, 4.0})}};
    const auto variants_of = [&](Json flat_buffer, Json xr_buffer, bool reversed) {
        auto flat_ir = irOf(images, Json::array({std::move(flat_buffer)}));
        auto xr_ir = irOf(images, Json::array({std::move(xr_buffer)}));
        std::vector<GenerationResourceVariant> variants{{.name = "flat", .render_graph = nullptr},
                                                        {.name = "xr", .render_graph = nullptr}};
        auto irs = std::make_shared<std::array<RenderGraphIR, 2>>(std::array{std::move(flat_ir), std::move(xr_ir)});
        variants[0].render_graph = &(*irs)[0];
        variants[1].render_graph = &(*irs)[1];
        if (reversed) std::swap(variants[0], variants[1]);
        return std::pair{irs, variants};
    };
    for (const bool reversed : {false, true}) {
        INFO("reversed " << reversed);
        const auto [same_irs, same] = variants_of(data, data, reversed);
        const auto agreed = resolveGenerationResources(same, {64, 64});
        REQUIRE(agreed.buffers.size() == 1);
        CHECK(agreed.buffers.front().initial_f32 == std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f});
        CHECK(agreed.buffers.front().host_source == FrameGraphHostBufferSource::runtime_f32);
        CHECK(agreed.variants[0].buffers == std::vector<std::size_t>{0});
        CHECK(agreed.variants[1].buffers == std::vector<std::size_t>{0});
        const std::string first = reversed ? "xr" : "flat";
        const std::string second = reversed ? "flat" : "xr";
        const auto refused = [&](Json changed, std::string_view field) {
            const auto [irs, variants] = variants_of(data, std::move(changed), reversed);
            CHECK(refusal([&] { return resolveGenerationResources(variants, {64, 64}); }) ==
                  std::string{R"(render_graph.buffer_variant_conflict@1 {"field":")"} + std::string{field} +
                      R"(","resource":"data","variants":[")" + first + R"(",")" + second + R"("]})");
        };
        auto changed = data;
        changed["initial_f32"] = Json::array({1.0, 2.0, 3.0, 5.0});
        refused(changed, "initial_f32");
        changed = data;
        changed["size"] = 32;
        changed.erase("initial_f32");
        refused(changed, "size");
        changed = data;
        changed["lifetime"] = "transient";
        refused(changed, "lifetime");
        changed = Json{{"name", "data"}, {"size", 16}, {"host_source", "scene_lights_v2"}};
        refused(changed, "host_source");
        changed = Json{{"name", "data"}, {"size", 16}, {"host_source", "runtime_f32"}, {"index_buffer", true},
                       {"initial_f32", Json::array({1.0, 2.0, 3.0, 4.0})}};
        refused(changed, "index_buffer");
        changed = Json{{"name", "data"}, {"size", 16}, {"command_layout", "compute_dispatch"}};
        refused(changed, "host_source");
        changed = Json{{"name", "data"}, {"size", 16}, {"host_source", "view_family_matrices_v1"},
                       {"view_family", "$project/capture"}};
        refused(changed, "host_source");
        changed = selection;
        changed["name"] = "data";
        refused(changed, "size");
    }
}

TEST_CASE("RC-3c2a a snapshot copy is sized by the images' extent rule",
          "[generation-resources][rc3c2a][snapshot]") {
    // (i) A third of the display's 1920x1080 is 640x360 by the float rule the
    // image is made with (the parse's own double arithmetic made 639x359).
    const auto config = Json::parse(R"json({
      "render_targets": [
        {"name": "display", "format_class": "display", "width": 1920, "height": 1080,
         "extent_scale": 1.0, "format": "B8G8R8A8_SRGB", "usage": ["COLOR_ATTACHMENT", "SAMPLED"]},
        {"name": "src", "extent_scale": 0.3333333333, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_SRC"]},
        {"name": "copy", "extent_scale": 0.3333333333, "format": "R8G8B8A8_UNORM",
         "usage": ["COLOR_ATTACHMENT", "SAMPLED", "TRANSFER_DST"]}],
      "rendering_passes": [{"name": "main", "passes": [
        {"name": "fill", "type": "fullscreen", "output": {"color": [{"target": "src"}], "depth": null}},
        {"name": "snap", "type": "snapshot_copy", "source": "src", "destination": "copy",
         "snapshot_after": "fill"}]}]})json");
    const auto ir = parseRenderGraph(config, PassFieldOwnershipCapabilities{});
    const auto &snapshot = std::get<SnapshotNodeInfo>(ir.graphs.at(0).nodes.at(1).info);
    CHECK(snapshot.byte_size == std::size_t{640} * 360 * 4);
    const std::array one{GenerationResourceVariant{.name = "flat", .render_graph = &ir}};
    CHECK(imageOf(resolveGenerationResources(one, {1920, 1080}), "copy").extent ==
          ResolvedResourceExtent{640, 360});
}

} // namespace Pelican
