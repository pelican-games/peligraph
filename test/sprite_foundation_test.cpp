#include "asset/atlasasset.hpp"
#include "components/spriteview.hpp"
#include "sprite/spriteworld.hpp"
#include "ui/atlas.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <numeric>
#include <type_traits>

using namespace Pelican;
using namespace Pelican::sprite;
using Json = nlohmann::json;

namespace {

SpriteViewComponent loadSpriteView(const Json &json) {
    SpriteViewComponent result;
    JsonArchiveLoader archive{static_cast<const void *>(&json)};
    result.ref(archive);
    return result;
}

SpriteCommand command(std::uint64_t ordinal, Bounds2 bounds = {-1.0f, -1.0f, 1.0f, 1.0f}) {
    SpriteCommand result;
    result.world_transform = {1.0f, 0.0f, 0.0f, 0.0f,
                              0.0f, 1.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.0f, 0.0f,
                              0.0f, 0.0f, 0.0f, 1.0f};
    result.uv_rect = {0.0f, 0.0f, 1.0f, 1.0f};
    result.source_entity = {7, 3};
    result.source_ordinal = ordinal;
    result.declaration_seq = ordinal;
    result.canvas_bounds = bounds;
    return result;
}

std::vector<SpriteCommand> commands(std::size_t count) {
    std::vector<SpriteCommand> result;
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) result.push_back(command(i));
    return result;
}

void requireContiguousOrdinals(const SpriteFrame &frame, std::size_t count) {
    std::size_t seen = 0;
    bool contiguous = true;
    for (const auto &chunk : frame.chunks) {
        REQUIRE(chunk.commands.size() <= maxQuadsPerChunk);
        REQUIRE(chunk.indices.size() == chunk.commands.size() * 6);
        for (const auto &value : chunk.commands) {
            contiguous = contiguous && value.source_ordinal == seen;
            ++seen;
        }
    }
    REQUIRE(contiguous);
    REQUIRE(seen == count);
}

} // namespace

TEST_CASE("AtlasAsset extraction preserves the U1 atlas document byte fields", "[sprite][atlas][u1]") {
    const auto source = std::filesystem::path{"fixture/hero.atlas.json"};
    const auto json = Json::parse(R"json({
      "schema":"pelican.atlas","version":1,
      "pages":[{"image":"page0.png","size":[16,8]},
                 {"image":"page1.png","size":[32,16]}],
      "sprites":{"z":{"page":1,"rect":[2,3,20,15]},
                 "a":{"page":0,"rect":[0,0,4,5]}}
    })json");

    const auto neutral = asset::parseAtlasAssetV1(json, source);
    const auto legacy = ui::parseAtlasV1(json, source);
    REQUIRE(neutral.source_path == legacy.source_path);
    REQUIRE(neutral.pages.size() == legacy.pages.size());
    REQUIRE(neutral.sprites.size() == legacy.sprites.size());
    for (std::size_t i = 0; i < neutral.pages.size(); ++i) {
        REQUIRE(neutral.pages[i].image_path == legacy.pages[i].image_path);
        REQUIRE(neutral.pages[i].size.width == legacy.pages[i].size.x);
        REQUIRE(neutral.pages[i].size.height == legacy.pages[i].size.y);
    }
    for (std::size_t i = 0; i < neutral.sprites.size(); ++i) {
        REQUIRE(neutral.sprites[i].name == legacy.sprites[i].name);
        REQUIRE(neutral.sprites[i].page == legacy.sprites[i].page);
        REQUIRE(neutral.sprites[i].rect.left == legacy.sprites[i].rect.left);
        REQUIRE(neutral.sprites[i].rect.top == legacy.sprites[i].rect.top);
        REQUIRE(neutral.sprites[i].rect.right == legacy.sprites[i].rect.right);
        REQUIRE(neutral.sprites[i].rect.bottom == legacy.sprites[i].rect.bottom);
    }
}

TEST_CASE("sprite_view schema is closed, finite, ranged, and has no sampler override",
          "[sprite][schema][c1]") {
    const auto loaded = loadSpriteView(Json::parse(R"json({
      "name":"sprite_view","texture":"hero#sprite/idle","size":[1.0,1.5],
      "pivot":[0.5,0.0],"color":[1.0,0.5,0.25,1.0],"flip":[true,false],
      "layer":-12,"billboard":"y_axis"
    })json"));
    REQUIRE(loaded.texture == "hero#sprite/idle");
    REQUIRE(loaded.has_explicit_size == 1);
    REQUIRE(loaded.size.x == 1.0f);
    REQUIRE(loaded.size.y == 1.5f);
    REQUIRE(loaded.pivot.x == 0.5f);
    REQUIRE(loaded.pivot.y == 0.0f);
    REQUIRE(loaded.flip_x == 1);
    REQUIRE(loaded.flip_y == 0);
    REQUIRE(loaded.layer == -12);
    REQUIRE(loaded.billboard == SpriteBillboard::y_axis);

    const auto defaults = loadSpriteView(
        Json{{"name", "sprite_view"}, {"texture", "standalone"}});
    REQUIRE(defaults.has_explicit_size == 0);
    REQUIRE(defaults.pivot.x == 0.5f);
    REQUIRE(defaults.pivot.y == 0.5f);
    REQUIRE(defaults.color.x == 1.0f);
    REQUIRE(defaults.color.y == 1.0f);
    REQUIRE(defaults.color.z == 1.0f);
    REQUIRE(defaults.color.w == 1.0f);
    REQUIRE(defaults.billboard == SpriteBillboard::none);

    REQUIRE_THROWS_WITH(
        loadSpriteView(Json{{"name", "sprite_view"}, {"texture", "hero"},
                            {"sampler", "nearest"}}),
        Catch::Matchers::ContainsSubstring("unknown field: sampler"));
    REQUIRE_THROWS(loadSpriteView(Json{{"name", "sprite_view"}, {"texture", "hero"},
                                       {"size", Json::array({0.0, 1.0})}}));
    REQUIRE_THROWS(loadSpriteView(Json{{"name", "sprite_view"}, {"texture", "hero"},
                                       {"pivot", Json::array({1.1, 0.5})}}));
    REQUIRE_THROWS(loadSpriteView(Json{{"name", "sprite_view"}, {"texture", "hero"},
                                       {"color", Json::array({1.0, 1.0,
                                                               std::numeric_limits<double>::infinity(), 1.0})}}));
    REQUIRE_THROWS(loadSpriteView(Json{{"name", "sprite_view"}, {"texture", "hero"},
                                       {"layer", 32768}}));
    REQUIRE_THROWS(loadSpriteView(Json{{"name", "sprite_view"}, {"texture", "hero"},
                                       {"billboard", "camera"}}));
    REQUIRE_THROWS(loadSpriteView(Json{{"name", "not_sprite"}, {"texture", "hero"}}));
    REQUIRE_THROWS(asset::parseSpriteAssetReference("snapshot:opaque_color"));
}

TEST_CASE("asset declarations own sampler and resolve atlas fragments", "[sprite][asset][c1]") {
    const auto assets = Json::parse(R"json({
      "models":[],
      "textures":[
        {"name":"hero","path":"assets/hero.atlas.json","sampler":"nearest"},
        {"name":"photo","path":"assets/photo.png"}
      ]
    })json");
    const asset::SpriteAssetCatalog catalog{assets};
    REQUIRE(catalog.declaration("hero").sampler == SamplerKey::nearest);
    REQUIRE(catalog.declaration("photo").sampler == SamplerKey::linear);

    const auto atlas_json = Json::parse(R"json({
      "schema":"pelican.atlas","version":1,
      "pages":[{"image":"hero.png","size":[64,32]}],
      "sprites":{"idle":{"page":0,"rect":[4,6,20,30]}}
    })json");
    const auto resolved = catalog.resolve(
        "hero#sprite/idle",
        [](std::string_view path) { return std::filesystem::path{path}; },
        [&](const std::filesystem::path &path) { return asset::parseAtlasAssetV1(atlas_json, path); });
    REQUIRE(resolved.declaration.sampler == SamplerKey::nearest);
    REQUIRE(resolved.page == 0);
    REQUIRE(resolved.rect == asset::AtlasRect{4, 6, 20, 30});
    REQUIRE(resolved.page_size == asset::AtlasExtent{64, 32});

    const auto standalone = catalog.resolve(
        "photo", [](std::string_view path) { return std::filesystem::path{path}; },
        [](const std::filesystem::path &) -> asset::AtlasAsset {
            FAIL("standalone images must not invoke the atlas parser");
            return {};
        });
    REQUIRE(standalone.source_path == std::filesystem::path{"assets/photo.png"});
    REQUIRE(standalone.declaration.sampler == SamplerKey::linear);

    auto bad_sampler = assets;
    bad_sampler["textures"][0]["sampler"] = "cubic";
    REQUIRE_THROWS_WITH(asset::SpriteAssetCatalog{bad_sampler},
                        Catch::Matchers::ContainsSubstring("nearest or linear"));
    auto unknown = assets;
    unknown["textures"][0]["wrap"] = "repeat";
    REQUIRE_THROWS_WITH(asset::SpriteAssetCatalog{unknown},
                        Catch::Matchers::ContainsSubstring("unknown field: wrap"));
    REQUIRE_THROWS(catalog.resolve(
        "hero#sprite/missing",
        [](std::string_view path) { return std::filesystem::path{path}; },
        [&](const std::filesystem::path &path) { return asset::parseAtlasAssetV1(atlas_json, path); }));
    REQUIRE_THROWS(catalog.resolve(
        "missing#sprite/idle",
        [](std::string_view path) { return std::filesystem::path{path}; },
        [&](const std::filesystem::path &path) { return asset::parseAtlasAssetV1(atlas_json, path); }));
}

TEST_CASE("sprite total order closes on full EntityId and source-local stable ordinal",
          "[sprite][sort][c2]") {
    REQUIRE(canonicalFloatKey(-0.0f) == canonicalFloatKey(0.0f));
    REQUIRE_THROWS(canonicalFloatKey(std::numeric_limits<float>::infinity()));

    StableOrdinalAllocator ordinals;
    const auto tree = ordinals.issue("tree");
    const auto rock = ordinals.issue("rock");
    REQUIRE(tree == 0);
    REQUIRE(rock == 1);
    ordinals.remove("tree");
    REQUIRE(ordinals.issue("tree") == tree);
    ordinals.resetForReplay();
    REQUIRE(ordinals.issue("tree") == tree);
    REQUIRE(ordinals.issue("rock") == rock);

    DeclarationSequenceAllocator declarations;
    REQUIRE(declarations.issue() == 0);
    REQUIRE(declarations.issue() == 1); // removal/recreate receives a fresh registration sequence.
    declarations.resetForReplay();
    REQUIRE(declarations.issue() == 0);

    auto first = command(9);
    first.source_entity = {4, 2};
    first.view_depth = -0.0f;
    auto second = command(2);
    second.source_entity = {4, 1};
    second.view_depth = 0.0f;
    auto third = command(1);
    third.source_entity = {4, 2};
    third.view_depth = 0.0f;
    auto frame = buildFrame({first, second, third}, {-2.0f, -2.0f, 2.0f, 2.0f}, SortPolicy::z);
    REQUIRE(frame.chunks[0].commands[0].source_entity == EntityId{4, 1});
    REQUIRE(frame.chunks[0].commands[1].source_ordinal == 1);
    REQUIRE(frame.chunks[0].commands[2].source_ordinal == 9);

    std::vector<SpriteCommand> canonical;
    for (const auto key : {"tile/c", "tile/a", "tile/b"}) {
        auto value = command(ordinals.issue(key));
        value.source_entity = {8, 5};
        canonical.push_back(value);
    }
    auto shuffled = canonical;
    std::reverse(shuffled.begin(), shuffled.end());
    const auto a = buildFrame(canonical, {-2.0f, -2.0f, 2.0f, 2.0f}, SortPolicy::z);
    const auto b = buildFrame(shuffled, {-2.0f, -2.0f, 2.0f, 2.0f}, SortPolicy::z);
    REQUIRE(commandOrderHash(a) == commandOrderHash(b));
    REQUIRE(a.chunks[0].commands == b.chunks[0].commands);

    auto near_z = command(1);
    near_z.view_depth = -1.0f;
    auto far_z = command(2);
    far_z.view_depth = -10.0f;
    REQUIRE(buildFrame({near_z, far_z}, {-2, -2, 2, 2}, SortPolicy::z)
                .chunks[0].commands.front().source_ordinal == 2);
    auto low_y = command(1);
    low_y.pivot_world_y = -5.0f;
    auto high_y = command(2);
    high_y.pivot_world_y = 5.0f;
    REQUIRE(buildFrame({low_y, high_y}, {-2, -2, 2, 2}, SortPolicy::y_down)
                .chunks[0].commands.front().source_ordinal == 2);
    auto declared_late = command(1);
    declared_late.declaration_seq = 9;
    auto declared_early = command(2);
    declared_early.declaration_seq = 3;
    REQUIRE(buildFrame({declared_late, declared_early}, {-2, -2, 2, 2},
                       SortPolicy::declaration).chunks[0].commands.front().source_ordinal == 2);
    auto lower_layer = command(3);
    lower_layer.layer = -1;
    auto higher_layer = command(0);
    higher_layer.layer = 1;
    REQUIRE(buildFrame({higher_layer, lower_layer}, {-2, -2, 2, 2}, SortPolicy::z)
                .chunks[0].commands.front().source_ordinal == 3);

    auto nearest = command(0);
    nearest.batch.sampler = SamplerKey::nearest;
    auto linear = nearest;
    linear.batch.sampler = SamplerKey::linear;
    REQUIRE(nearest.batch != linear.batch); // sampler is part of the immutable batch key.
    REQUIRE(commandOrderHash(buildFrame({nearest}, {-2, -2, 2, 2}, SortPolicy::z)) !=
            commandOrderHash(buildFrame({linear}, {-2, -2, 2, 2}, SortPolicy::z)));

    auto invalid = command(0);
    invalid.world_transform[0] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS(buildFrame({invalid}, {-2, -2, 2, 2}, SortPolicy::z));
}

TEST_CASE("sprite chunks cross 16384 without loss, duplication, or uint16 overflow",
          "[sprite][chunk][c3]") {
    STATIC_REQUIRE(std::is_same_v<typename decltype(SpriteChunk::indices)::value_type,
                                  std::uint16_t>);
    const Bounds2 view{-2.0f, -2.0f, 2.0f, 2.0f};
    for (const auto count : {std::size_t{16384}, std::size_t{16385}, std::size_t{32769}}) {
        auto input = commands(count);
        auto reversed = input;
        std::reverse(reversed.begin(), reversed.end());
        const auto frame = buildFrame(std::move(input), view, SortPolicy::z);
        REQUIRE(frame.logical_count == count);
        REQUIRE(frame.visible_count == count);
        REQUIRE(frame.chunks.size() == (count + maxQuadsPerChunk - 1) / maxQuadsPerChunk);
        requireContiguousOrdinals(frame, count);
        if (count >= maxQuadsPerChunk) REQUIRE(frame.chunks.front().indices.back() == UINT16_MAX);
        const auto rerun = buildFrame(std::move(reversed), view, SortPolicy::z);
        REQUIRE(commandOrderHash(frame) == commandOrderHash(rerun));
    }
}

TEST_CASE("50k logical sprites cull to 2k and static revisions control cache reuse",
          "[sprite][scale][cache]") {
    constexpr std::size_t logical = 50000;
    constexpr std::size_t visible_count = 2000;
    const Bounds2 view{-2.0f, -2.0f, 2.0f, 2.0f};
    std::vector<SpriteCommand> input;
    input.reserve(logical);
    for (std::size_t i = 0; i < logical; ++i) {
        const auto bounds = i < visible_count ? Bounds2{-1, -1, 1, 1}
                                              : Bounds2{10, 10, 11, 11};
        input.push_back(command(i, bounds));
    }

    constexpr int samples = 5;
    double total_ms = 0.0;
    double min_ms = std::numeric_limits<double>::max();
    double max_ms = 0.0;
    std::uint64_t expected_hash = 0;
    for (int sample = 0; sample < samples; ++sample) {
        const auto start = std::chrono::steady_clock::now();
        const auto frame = buildFrame(input, view, SortPolicy::z);
        const auto elapsed = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start).count();
        total_ms += elapsed;
        min_ms = std::min(min_ms, elapsed);
        max_ms = std::max(max_ms, elapsed);
        REQUIRE(frame.logical_count == logical);
        REQUIRE(frame.visible_count == visible_count);
        REQUIRE(frame.chunks.size() == 1);
        REQUIRE(frame.chunks[0].commands.front().source_ordinal == 0);
        REQUIRE(frame.chunks[0].commands.back().source_ordinal == visible_count - 1);
        const auto hash = commandOrderHash(frame);
        if (sample == 0) expected_hash = hash;
        REQUIRE(hash == expected_hash);
    }
    std::cout << "sprite_cpu: samples=" << samples << " logical=" << logical
              << " visible=" << visible_count << " avg_ms=" << (total_ms / samples)
              << " min_ms=" << min_ms << " max_ms=" << max_ms
              << " order_hash=" << expected_hash << '\n';

    StaticChunkCache cache;
    const SpriteRevisions revisions{1, 2, 3, 4};
    const auto miss = buildFrame(input, view, SortPolicy::z, &cache, revisions);
    REQUIRE_FALSE(miss.cache_hit);
    auto changed_input = input;
    changed_input[0].source_ordinal = 999999;
    const auto hit = buildFrame(changed_input, view, SortPolicy::z, &cache, revisions);
    REQUIRE(hit.cache_hit);
    REQUIRE(commandOrderHash(hit) == commandOrderHash(miss));
    auto changed_revisions = revisions;
    ++changed_revisions.source;
    const auto invalidated = buildFrame(std::move(changed_input), view, SortPolicy::z,
                                        &cache, changed_revisions);
    REQUIRE_FALSE(invalidated.cache_hit);
    REQUIRE(commandOrderHash(invalidated) != commandOrderHash(miss));
}
