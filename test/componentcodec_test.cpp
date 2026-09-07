#include "../src/core/ecs/predefined/transform.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/renderer/proceduralgeometrycomponent.hpp"
#include "../src/core/userpublic/components/animation.hpp"
#include "../src/core/userpublic/components/collider.hpp"
#include "../src/core/userpublic/components/localtransform.hpp"
#include "../src/core/userpublic/components/modelview.hpp"
#include "../src/core/userpublic/components/spriteview.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <any>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>

#ifndef PELICAN_TEST_SOURCE_DIR
#define PELICAN_TEST_SOURCE_DIR "."
#endif

namespace Pelican {
namespace {

nlohmann::json readFixture(std::string_view name) {
    const auto path = std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / "test" / "fixtures" /
                      "component_codec" / name;
    std::ifstream input{path, std::ios_base::binary};
    if (!input.is_open()) throw std::runtime_error("failed to open fixture: " + path.string());
    return nlohmann::json::parse(input);
}

std::string readRepositoryText(const std::filesystem::path &relative) {
    const auto path =
        std::filesystem::path{PELICAN_TEST_SOURCE_DIR} / relative;
    std::ifstream input{path, std::ios_base::binary};
    if (!input.is_open())
        throw std::runtime_error("failed to open fixture: " + path.string());
    std::string text{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
    // The generated document is compared byte-for-byte with the registry
    // output; a checkout with core.autocrlf must not turn that into a
    // line-ending diff.
    std::erase(text, '\r');
    return text;
}

struct ResolutionSentinelValue {};
struct ResolutionSentinelError {};
using SentinelResolutionResult =
    ResolutionResult<ResolutionSentinelValue, ResolutionSentinelError>;
static_assert(!std::is_copy_constructible_v<SentinelResolutionResult>);
static_assert(!std::is_copy_assignable_v<SentinelResolutionResult>);
static_assert(std::is_move_constructible_v<SentinelResolutionResult>);
static_assert(std::is_move_assignable_v<SentinelResolutionResult>);
static_assert(
    !std::is_constructible_v<std::any, SentinelResolutionResult &>);
static_assert(
    !std::is_constructible_v<std::any, SentinelResolutionResult &&>);
static_assert(!std::is_assignable_v<std::any &, SentinelResolutionResult &>);
static_assert(!std::is_assignable_v<std::any &, SentinelResolutionResult &&>);

ComponentCodecValue decodeOrThrow(const ComponentCodec &codec,
                                  const nlohmann::json &authored) {
    auto decoded = codec.decodeAuthored(authored);
    if (!decoded) {
        throw ResolutionFailureException{
            StoredResolutionFailure{std::move(decoded).error()}};
    }
    return std::move(decoded).value();
}

nlohmann::ordered_json canonicalThroughRuntime(const ComponentCodec &codec,
                                               const nlohmann::json &authored) {
    const auto decoded = decodeOrThrow(codec, authored);
    ComponentCodecValue projected;
    if (codec.name == "transform") {
        TransformComponent world{};
        LocalTransformComponent local{};
        TransformCodecTarget target{.world = &world, .local = &local};
        codec.applyRuntime(decoded, &target);
        projected = codec.projectRuntime(&target);
    } else if (codec.name == "simplemodelview") {
        SimpleModelViewComponent runtime;
        codec.applyRuntime(decoded, &runtime);
        projected = codec.projectRuntime(&runtime);
    } else if (codec.name == "camera") {
        CameraCodecData runtime;
        codec.applyRuntime(decoded, &runtime);
        projected = codec.projectRuntime(&runtime);
    } else if (codec.name == "light") {
        LightCodecData runtime;
        codec.applyRuntime(decoded, &runtime);
        projected = codec.projectRuntime(&runtime);
    } else if (codec.name == "collider") {
        ColliderComponent runtime;
        codec.applyRuntime(decoded, &runtime);
        projected = codec.projectRuntime(&runtime);
    } else if (codec.name == "animation") {
        AnimationComponent runtime;
        codec.applyRuntime(decoded, &runtime);
        projected = codec.projectRuntime(&runtime);
    } else if (codec.name == "sprite_view") {
        SpriteViewComponent runtime;
        codec.applyRuntime(decoded, &runtime);
        projected = codec.projectRuntime(&runtime);
    } else if (codec.name == "pelican_procedural_geometry") {
        ProceduralGeometryComponent runtime;
        codec.applyRuntime(decoded, &runtime);
        projected = codec.projectRuntime(&runtime);
    } else {
        throw std::logic_error("unhandled codec in fixture");
    }
    return codec.encodeCanonical(projected);
}

void requireVec3(glm::vec3 actual, glm::vec3 expected) {
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(1.0e-5f));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(1.0e-5f));
    REQUIRE(actual.z == Catch::Approx(expected.z).margin(1.0e-5f));
}

} // namespace

TEST_CASE("component codec registry exposes runtime and editor projection policy", "[component-codec][wp151][WP375]") {
    const auto codecs = componentCodecs();
    REQUIRE(codecs.size() == 8);
    std::unordered_set<std::string_view> names;
    for (const auto &codec : codecs) {
        REQUIRE(names.insert(codec.name).second);
        REQUIRE(codec.decode_authored != nullptr);
        REQUIRE(codec.encode_canonical != nullptr);
        REQUIRE(codec.schema != nullptr);
        REQUIRE(codec.runtime_apply != nullptr);
        REQUIRE(codec.runtime_project != nullptr);
        if (codec.editor_policy == EditorProjectionPolicy::EcsCodec) {
            REQUIRE(codec.editor_prepare != nullptr);
            REQUIRE(codec.editor_swap != nullptr);
        } else {
            REQUIRE(codec.editor_prepare == nullptr);
            REQUIRE(codec.editor_swap == nullptr);
        }
        REQUIRE_FALSE(codec.fieldSchema().empty());
        const auto metadata = componentCodecQueryMetadata(codec.name);
        REQUIRE(metadata.state == ComponentCodecState::Registered);
        REQUIRE(metadata.editable);
        REQUIRE(std::string{metadata.codec_name} == std::string{codec.name});
    }
    const auto missing = componentCodecQueryMetadata("unknown_read_only");
    REQUIRE(missing.state == ComponentCodecState::Missing);
    REQUIRE_FALSE(missing.editable);
    REQUIRE(missing.codec_name.empty());
}

TEST_CASE("WP378 legacy descriptor corpus is a configuration-independent bijection",
          "[component-codec][WP378][legacy-corpus][mutation]") {
    const auto descriptors = componentCodecLegacyBranches();
    const auto corpus = readFixture("legacy_corpus.json");
    REQUIRE(corpus.size() == descriptors.size());

    std::set<std::string> descriptor_ids;
    std::ostringstream generated;
    generated << "# Component codec legacy branches\n\n"
                 "Generated from `componentCodecLegacyBranches()`; edit the "
                 "production registry and its reachability corpus together.\n\n"
                 "| legacy_branch_id | producer |\n"
                 "|---|---|\n";
    for (const auto &descriptor : descriptors) {
        REQUIRE(descriptor_ids.emplace(descriptor.legacy_branch_id).second);
        generated << "| `" << descriptor.legacy_branch_id << "` | `"
                  << descriptor.producer << "` |\n";
    }

    std::set<std::string> reached_ids;
    for (const auto &entry : corpus) {
        const auto expected =
            entry.at("legacy_branch_id").get<std::string>();
        const auto codec_name = entry.at("codec").get<std::string>();
        INFO("legacy branch " << expected << " via " << codec_name);
        const auto *codec =
            testing::findComponentCodecIncludingDisabled(codec_name);
        REQUIRE(codec != nullptr);
        try {
            auto result = codec->decodeAuthored(entry.at("authored"));
            if (!result) {
                FAIL("legacy corpus entry returned a typed violation");
            }
            FAIL("legacy corpus entry decoded successfully");
        } catch (const LegacyRejectException &error) {
            REQUIRE(error.legacyBranchId() == expected);
            REQUIRE(reached_ids.insert(error.legacyBranchId()).second);
        } catch (const nlohmann::json::type_error &error) {
            FAIL("naked nlohmann::json::type_error: " << error.what());
        }
    }
    REQUIRE(reached_ids == descriptor_ids);
    REQUIRE(readRepositoryText(
                "docs/component_codec_legacy_branches.md") ==
            generated.str());
}

TEST_CASE("eight component codecs roundtrip authored runtime canonical and fresh load",
          "[component-codec][roundtrip][wp151]") {
    for (const auto &entry : readFixture("valid.json")) {
        const auto name = entry.at("name").get<std::string>();
        DYNAMIC_SECTION(name) {
            const auto &codec = requireComponentCodec(name);
            const auto canonical = canonicalThroughRuntime(codec, entry.at("authored"));
            REQUIRE(nlohmann::json::parse(canonical.dump()) == entry.at("canonical"));
            REQUIRE(canonicalThroughRuntime(codec, canonical) == canonical);
        }
    }
}

TEST_CASE("transform codec keeps authored local TRS and projects parent-child world TRS",
          "[component-codec][transform][hierarchy][wp151]") {
    const auto &codec = requireComponentCodec("transform");
    const auto parent_json = nlohmann::json{
        {"name", "transform"}, {"pos", {10.0f, 0.0f, 0.0f}},
        {"rotation", {0.0f, 0.0f, 0.70710678118f, 0.70710678118f}},
        {"scale", {2.0f, 3.0f, 4.0f}},
    };
    const auto child_json = nlohmann::json{
        {"name", "transform"}, {"pos", {1.0f, 2.0f, 3.0f}},
        {"rotation", {0.0f, 0.0f, 0.0f, 1.0f}}, {"scale", {0.5f, 2.0f, 1.0f}},
    };

    TransformComponent parent_world{};
    TransformCodecTarget parent_target{.world = &parent_world};
    codec.applyRuntime(decodeOrThrow(codec, parent_json), &parent_target);

    TransformComponent child_world{};
    LocalTransformComponent child_local{};
    TransformCodecTarget child_target{
        .world = &child_world, .local = &child_local, .parent_world = &parent_world};
    codec.applyRuntime(decodeOrThrow(codec, child_json), &child_target);

    requireVec3(child_world.pos, {4.0f, 2.0f, 12.0f});
    requireVec3(child_world.scale, {1.0f, 6.0f, 4.0f});
    const auto runtime_json = projectTransformRuntimeJson(child_target);
    REQUIRE(runtime_json.at("local_trs") ==
            codec.encodeCanonical(decodeOrThrow(codec, child_json)));
    const auto &world_pos = runtime_json.at("world_trs").at("pos");
    REQUIRE(world_pos.at(0).get<float>() == Catch::Approx(4.0f).margin(1.0e-5f));
    REQUIRE(world_pos.at(1).get<float>() == Catch::Approx(2.0f).margin(1.0e-5f));
    REQUIRE(world_pos.at(2).get<float>() == Catch::Approx(12.0f).margin(1.0e-5f));

    TransformCodecTarget inverse_target{.world = &child_world, .parent_world = &parent_world};
    const auto inverse_local = codec.encodeCanonical(codec.projectRuntime(&inverse_target));
    const auto expected_local =
        codec.encodeCanonical(decodeOrThrow(codec, child_json));
    for (const auto *field : {"pos", "rotation", "scale"}) {
        for (std::size_t index = 0; index < inverse_local.at(field).size(); ++index) {
            REQUIRE(inverse_local.at(field).at(index).get<float>() ==
                    Catch::Approx(expected_local.at(field).at(index).get<float>()).margin(1.0e-5f));
        }
    }

    TransformComponent fresh_world{};
    LocalTransformComponent fresh_local{};
    TransformCodecTarget fresh_target{
        .world = &fresh_world, .local = &fresh_local, .parent_world = &parent_world};
    codec.applyRuntime(
        decodeOrThrow(codec, runtime_json.at("local_trs")), &fresh_target);
    requireVec3(fresh_world.pos, child_world.pos);
    requireVec3(fresh_world.scale, child_world.scale);
}

TEST_CASE("animation and sprite authored booleans remain JSON booleans after runtime projection",
          "[component-codec][boolean][wp151]") {
    const auto valid = readFixture("valid.json");
    for (const auto &entry : valid) {
        const auto name = entry.at("name").get<std::string>();
        if (name == "animation") {
            const auto canonical = canonicalThroughRuntime(requireComponentCodec(name), entry.at("authored"));
            REQUIRE(canonical.at("loop").is_boolean());
            REQUIRE_FALSE(canonical.at("loop").get<bool>());
        } else if (name == "sprite_view") {
            const auto canonical = canonicalThroughRuntime(requireComponentCodec(name), entry.at("authored"));
            REQUIRE(canonical.at("flip").is_array());
            REQUIRE(canonical.at("flip").at(0).is_boolean());
            REQUIRE(canonical.at("flip").at(1).is_boolean());
            REQUIRE(canonical.at("billboard") == "y_axis");
        }
    }
}

TEST_CASE("collider trigger is an additive default-false boolean schema field",
          "[component-codec][collider][trigger][wp179]") {
    const auto &codec = requireComponentCodec("collider");
    const auto fields = codec.fieldSchema();
    const auto trigger = std::find_if(fields.begin(), fields.end(), [](const auto &field) {
        return field.name == "trigger";
    });
    REQUIRE(trigger != fields.end());
    REQUIRE(trigger->type == StructFieldType::Bool);

    const auto canonical = canonicalThroughRuntime(
        codec, nlohmann::json{{"name", "collider"},
                              {"shape", "sphere"},
                              {"radius", 1.0f}});
    REQUIRE(canonical.at("trigger").is_boolean());
    REQUIRE_FALSE(canonical.at("trigger").get<bool>());
}

TEST_CASE("animation graph authoring roundtrips one source without clip clock fields",
          "[component-codec][animation][graph]") {
    using Json = nlohmann::json;
    const auto &codec = requireComponentCodec("animation");
    const Json authored{{"name", "animation"}, {"graph", "graphs/locomotion.json"}};
    const auto decoded = decodeOrThrow(codec, authored);
    AnimationComponent runtime;
    codec.applyRuntime(decoded, &runtime);
    CHECK(runtime.clip.empty());
    CHECK(runtime.graph == "graphs/locomotion.json");
    CHECK(Json(codec.encodeCanonical(codec.projectRuntime(&runtime))) == authored);
    const auto &schema = codec.schemaNode();
    CHECK(std::string{schema.at(schema.childIndex(0, 0)).name} == "clip");
    CHECK(std::string{schema.at(schema.childIndex(0, 1)).name} == "graph");
    CHECK(schema.at(schema.childIndex(0, 0)).presence == StructFieldPresence::OptionalOverride);
    CHECK(schema.at(schema.childIndex(0, 1)).presence == StructFieldPresence::OptionalOverride);
    for (const auto &fields : std::vector<Json>{
             Json::object(), {{"graph", ""}}, {{"graph", 4}},
             {{"clip", "Walk"}, {"graph", "walk.json"}},
             {{"graph", "walk.json"}, {"speed", 1.0}},
             {{"graph", "walk.json"}, {"loop", true}},
             {{"graph", "walk.json"}, {"start_time", 0.0}}}) {
        auto invalid = fields;
        invalid["name"] = "animation";
        CHECK_THROWS(decodeOrThrow(codec, invalid));
    }
    // Existing clip documents retain their exact canonical shape.
    const Json clip{{"name", "animation"}, {"clip", "Walk"},
                    {"speed", 1.0}, {"loop", true}, {"start_time", 0.0}};
    CHECK(Json(canonicalThroughRuntime(codec, clip)) == clip);
    auto with_parameters = authored;
    with_parameters["parameters"] = Json::parse(R"({"$list":{"v":1,"next_id":2},
        "rows":[{"$row":{"id":1,"v":1},"parameter":"speed","value":0.75}]})");
    CHECK(Json(canonicalThroughRuntime(codec, with_parameters)) == with_parameters);
    auto duplicate = with_parameters;
    duplicate["parameters"]["rows"].push_back(duplicate["parameters"]["rows"][0]);
    duplicate["parameters"]["rows"][1]["$row"]["id"] = 2;
    duplicate["parameters"]["$list"]["next_id"] = 3;
    CHECK_THROWS(decodeOrThrow(codec, duplicate));
    auto clip_parameters = clip;
    clip_parameters["parameters"] = with_parameters["parameters"];
    CHECK_THROWS(decodeOrThrow(codec, clip_parameters));
}

TEST_CASE("every component codec rejects invalid type range and unknown key fixtures",
          "[component-codec][invalid][wp151]") {
    std::unordered_set<std::string> covered;
    for (const auto &entry : readFixture("invalid.json")) {
        const auto name = entry.at("name").get<std::string>();
        const auto kind = entry.at("kind").get<std::string>();
        DYNAMIC_SECTION(name << "/" << kind) {
            REQUIRE_THROWS(
                decodeOrThrow(requireComponentCodec(name),
                              entry.at("authored")));
        }
        covered.insert(name + "/" + kind);
    }
    for (const auto &codec : componentCodecs()) {
        for (const auto kind : {"type", "range", "unknown"}) {
            REQUIRE(covered.contains(std::string{codec.name} + "/" + kind));
        }
    }
}

TEST_CASE("procedural geometry codec normalizes a single asset or a unique asset list",
          "[component-codec][procedural][multi-aabb]") {
    using Json = nlohmann::json;
    const auto *codec = findComponentCodec("pelican_procedural_geometry");
    REQUIRE(codec);
    const Json single{{"name", "pelican_procedural_geometry"}, {"asset", "left"}};
    CHECK(Json(codec->encodeCanonical(decodeOrThrow(*codec, single))) == single);
    CHECK(Json(codec->encodeCanonical(decodeOrThrow(*codec,
        {{"name", "pelican_procedural_geometry"}, {"assets", {"left"}}}))) == single);
    const Json multiple{{"name", "pelican_procedural_geometry"}, {"assets", {"left", "right"}}};
    const auto decoded = decodeOrThrow(*codec, multiple);
    ProceduralGeometryComponent runtime;
    codec->applyRuntime(decoded, &runtime);
    CHECK(runtime.assets == std::vector<std::string>{"left", "right"});
    CHECK(Json(codec->encodeCanonical(codec->projectRuntime(&runtime))) == multiple);
    const auto &schema = codec->schemaNode();
    REQUIRE(schema.childCount(0) == 2);
    CHECK(schema.at(schema.childIndex(0, 1)).tag == SchemaNodeTag::List);
    CHECK(schema.at(schema.childIndex(0, 0)).presence == StructFieldPresence::OptionalOverride);
    for (const auto &fields : std::vector<Json>{Json::object(), {{"asset", ""}},
             {{"asset", "left"}, {"assets", {"right"}}}, {{"assets", Json::array()}},
             {{"assets", "left"}}, {{"assets", {"left", "left"}}}, {{"assets", {"left", 1}}}}) {
        auto authored = fields;
        authored["name"] = "pelican_procedural_geometry";
        CHECK_THROWS(decodeOrThrow(*codec, authored));
    }
}

} // namespace Pelican
