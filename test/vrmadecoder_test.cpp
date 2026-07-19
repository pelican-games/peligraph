#include "../src/core/loader/vrmadecoder.hpp"
#include "vrma_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <picosha2.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>

namespace Pelican {
namespace {

VrmaDecodeOptions provenance(std::string source = "asset://mocap/combined.vrma") {
    return {
        .source_uri = std::move(source),
        .import_profile = "vrma-c0/default@1",
        .tool_version = "pelican-fixture-dcc/2.4.1",
    };
}

std::shared_ptr<const VrmaClip> decode(TestVrmaFixture::Kind kind,
                                       std::string source = "asset://mocap/test.vrma") {
    const auto bytes = TestVrmaFixture::makeGlb(kind);
    return decodeVrmaAnimation(bytes, "fixture.vrma", provenance(std::move(source)));
}

const VrmaBodyChannel &bodyChannel(const VrmaClip &clip, std::string_view bone,
                                   VrmaBodyPath path) {
    const auto found = std::find_if(
        clip.body_channels.begin(), clip.body_channels.end(),
        [&](const VrmaBodyChannel &channel) {
            return channel.human_bone == bone && channel.path == path;
        });
    if (found == clip.body_channels.end()) throw std::runtime_error("body channel not found");
    return *found;
}

template <class T>
concept HasRootMotionMember = requires(T value) { value.root_motion; };

static_assert(std::is_same_v<decltype(decodeVrmaAnimation(
                                 std::span<const std::uint8_t>{}, std::string{},
                                 VrmaDecodeOptions{})),
                             std::shared_ptr<const VrmaClip>>);
static_assert(!HasRootMotionMember<VrmaClip>);
static_assert(!std::is_same_v<VrmaBodyChannel, VrmaExpressionChannel>);
static_assert(!std::is_same_v<VrmaBodyChannel, VrmaGazeChannel>);

} // namespace

TEST_CASE("VRMA-C0 decodes first animation into body expression and gaze channels",
          "[vrma]") {
    const auto bytes = TestVrmaFixture::makeGlb(TestVrmaFixture::Kind::full);
    const auto clip = decodeVrmaAnimation(bytes, "combined.vrma", provenance());

    REQUIRE(clip);
    REQUIRE(clip->spec_version == "1.0");
    REQUIRE(clip->name == "#animation/0");
    REQUIRE(clip->metadata.source_uri == "asset://mocap/combined.vrma");
    REQUIRE(clip->metadata.source_fragment == "#animation/0");
    REQUIRE(clip->metadata.content_sha256 ==
            picosha2::hash256_hex_string(bytes.begin(), bytes.end()));
    REQUIRE(clip->metadata.import_profile == "vrma-c0/default@1");
    REQUIRE(clip->metadata.tool_version == "pelican-fixture-dcc/2.4.1");
    REQUIRE(clip->start == 0.0f);
    REQUIRE(clip->end == 1.0f);

    REQUIRE(clip->source_rig);
    REQUIRE(clip->source_rig->human_bones.size() == 15);
    REQUIRE(clip->body_channels.size() == 2);
    const auto &hips = bodyChannel(*clip, "hips", VrmaBodyPath::translation);
    REQUIRE(hips.track.component_count == 3);
    REQUIRE(hips.track.values.size() == 2);
    // Translation is retained as body data. VRMA-C0 performs no root-motion extraction.
    REQUIRE(hips.track.values[1][0] == 0.25f);
    REQUIRE(bodyChannel(*clip, "spine", VrmaBodyPath::rotation)
                .track.component_count == 4);

    REQUIRE(clip->expression_channels.size() == 1);
    REQUIRE(clip->expression_channels[0].expression == "happy");
    REQUIRE(clip->expression_channels[0].preset);
    REQUIRE(clip->expression_channels[0].weight.values ==
            std::vector<float>{0.0f, 1.0f});

    REQUIRE(clip->gaze_channel);
    REQUIRE(clip->gaze_channel->rotation.component_count == 4);
    REQUIRE(clip->gaze_channel->offset_from_head_bone ==
            std::optional<std::array<float, 3>>{{0.0f, 0.06f, 0.0f}});
}

TEST_CASE("VRMA-C0 supports each typed channel inventory independently", "[vrma]") {
    SECTION("body only") {
        const auto clip = decode(TestVrmaFixture::Kind::body_only);
        REQUIRE(clip->body_channels.size() == 2);
        REQUIRE(clip->expression_channels.empty());
        REQUIRE_FALSE(clip->gaze_channel);
    }
    SECTION("expression only") {
        const auto clip = decode(TestVrmaFixture::Kind::expression_only);
        REQUIRE(clip->body_channels.empty());
        REQUIRE(clip->expression_channels.size() == 1);
        REQUIRE_FALSE(clip->gaze_channel);
    }
    SECTION("gaze only") {
        const auto clip = decode(TestVrmaFixture::Kind::gaze_only);
        REQUIRE(clip->body_channels.empty());
        REQUIRE(clip->expression_channels.empty());
        REQUIRE(clip->gaze_channel);
    }
    SECTION("all channels absent") {
        const auto clip = decode(TestVrmaFixture::Kind::empty);
        REQUIRE(clip->body_channels.empty());
        REQUIRE(clip->expression_channels.empty());
        REQUIRE_FALSE(clip->gaze_channel);
        REQUIRE(clip->start == 0.0f);
        REQUIRE(clip->end == 0.0f);
    }
}

TEST_CASE("VRMA fixture file round-trips through exact-byte provenance", "[vrma]") {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      ("pelican_wp176_" + std::to_string(stamp) + ".vrma");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{path};
    TestVrmaFixture::writeGlb(path, TestVrmaFixture::Kind::full);

    const auto from_file = loadVrmaAnimation(
        path, provenance("asset://library/walk_cycle.vrma"));
    const auto from_memory = decode(TestVrmaFixture::Kind::full,
                                    "asset://library/walk_cycle.vrma");
    REQUIRE(from_file->metadata.content_sha256 == from_memory->metadata.content_sha256);
    REQUIRE(from_file->metadata.source_uri == "asset://library/walk_cycle.vrma");
    REQUIRE(from_file->body_channels.size() == from_memory->body_channels.size());
    REQUIRE(from_file->expression_channels[0].weight.values ==
            from_memory->expression_channels[0].weight.values);
}

TEST_CASE("VRMA-C0 rejects named version mapping and channel violations", "[vrma]") {
    REQUIRE_THROWS_WITH(
        decode(TestVrmaFixture::Kind::unsupported_version, "asset://bad/future.vrma"),
        Catch::Matchers::ContainsSubstring("future.vrma") &&
            Catch::Matchers::ContainsSubstring(
                "unsupported VRMC_vrm_animation specVersion '1.1'"));
    REQUIRE_THROWS_WITH(
        decode(TestVrmaFixture::Kind::invalid_channel_type,
               "asset://bad/expression_path.vrma"),
        Catch::Matchers::ContainsSubstring("expression_path.vrma") &&
            Catch::Matchers::ContainsSubstring("invalid expression channel path 'rotation'") &&
            Catch::Matchers::ContainsSubstring("happy"));
    REQUIRE_THROWS_WITH(
        decode(TestVrmaFixture::Kind::missing_humanoid_map,
               "asset://bad/missing_hips.vrma"),
        Catch::Matchers::ContainsSubstring("missing_hips.vrma") &&
            Catch::Matchers::ContainsSubstring("missing required bone 'hips'"));
    REQUIRE_THROWS_WITH(
        decode(TestVrmaFixture::Kind::unknown_channel,
               "asset://bad/unmapped_channel.vrma"),
        Catch::Matchers::ContainsSubstring("unmapped_channel.vrma") &&
            Catch::Matchers::ContainsSubstring("without a VRMC_vrm_animation") &&
            Catch::Matchers::ContainsSubstring("channel[4]"));
}

TEST_CASE("VRMA alias and provenance gates reject non-conforming inputs", "[vrma]") {
    const auto valid = TestVrmaFixture::makeGlb(TestVrmaFixture::Kind::full);
    REQUIRE_THROWS_WITH(
        decodeVrmaAnimation(valid, "renamed.glb", provenance("asset://bad/renamed.glb")),
        Catch::Matchers::ContainsSubstring("renamed.glb") &&
            Catch::Matchers::ContainsSubstring(".vrma alias requires"));
    REQUIRE_THROWS_WITH(
        decodeVrmaAnimation(TestVrmaFixture::makeNonGlbAlias(), "text.vrma",
                            provenance("asset://bad/text.vrma")),
        Catch::Matchers::ContainsSubstring("text.vrma") &&
            Catch::Matchers::ContainsSubstring("GLB magic missing"));
    auto no_profile = provenance();
    no_profile.import_profile.clear();
    REQUIRE_THROWS_WITH(
        decodeVrmaAnimation(valid, "combined.vrma", std::move(no_profile)),
        Catch::Matchers::ContainsSubstring("import_profile must not be empty"));
    auto no_tool = provenance();
    no_tool.tool_version.clear();
    REQUIRE_THROWS_WITH(
        decodeVrmaAnimation(valid, "combined.vrma", std::move(no_tool)),
        Catch::Matchers::ContainsSubstring("tool_version must not be empty"));
}

} // namespace Pelican
