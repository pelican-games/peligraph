#include "../src/core/userpublic/details/schema/structfieldjson.hpp"
#include "../src/project/jsondocument.hpp"
#include "../src/core/userpublic/details/schema/jsonnumber.hpp"
#include <bit>
#include <limits>
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

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <any>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

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

std::vector<std::string> keysOf(const nlohmann::ordered_json &object) {
    std::vector<std::string> result;
    for (const auto &entry : object.items()) result.push_back(entry.key());
    return result;
}

// A declared schema name can describe a nested editor path ("sprite.sort") or
// an element of a packed wire value ("flip[0]"); the accepted document key is
// the leading segment in both cases.
std::string wireKeyOf(std::string_view schema_field) {
    const auto end = schema_field.find_first_of(".[");
    return std::string{schema_field.substr(0, end)};
}

void requireVec3(glm::vec3 actual, glm::vec3 expected) {
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(1.0e-5f));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(1.0e-5f));
    REQUIRE(actual.z == Catch::Approx(expected.z).margin(1.0e-5f));
}

void requireQuat(glm::quat actual, glm::quat expected) {
    REQUIRE(actual.x == Catch::Approx(expected.x).margin(1.0e-6f));
    REQUIRE(actual.y == Catch::Approx(expected.y).margin(1.0e-6f));
    REQUIRE(actual.z == Catch::Approx(expected.z).margin(1.0e-6f));
    REQUIRE(actual.w == Catch::Approx(expected.w).margin(1.0e-6f));
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

TEST_CASE("transform codec normalizes the world rotation of a near-unit authored quaternion",
          "[component-codec][transform][hierarchy][wp151]") {
    const auto &codec = requireComponentCodec("transform");
    // projects/example authors 0.707 rather than 0.7071068, so |q| is 0.99985.
    // Runtime used to publish that raw quaternion while the editor preview
    // published the normalized one.
    const auto local_json = nlohmann::json{
        {"name", "transform"}, {"pos", {0.0f, 0.0f, 0.0f}},
        {"rotation", {0.0f, 0.707f, -0.707f, 0.0f}},
        {"scale", {1.0f, 1.0f, 1.0f}},
    };
    const auto local_rotation =
        glm::normalize(glm::quat{0.0f, 0.0f, 0.707f, -0.707f});

    TransformComponent root_world{};
    TransformCodecTarget root_target{.world = &root_world};
    codec.applyRuntime(decodeOrThrow(codec, local_json), &root_target);
    REQUIRE(glm::length(root_world.rotation) ==
            Catch::Approx(1.0f).margin(1.0e-6f));
    requireQuat(root_world.rotation, local_rotation);

    const auto parent_json = nlohmann::json{
        {"name", "transform"}, {"pos", {10.0f, 0.0f, 0.0f}},
        {"rotation", {0.0f, 0.0f, 0.70710678118f, 0.70710678118f}},
        {"scale", {2.0f, 3.0f, 4.0f}},
    };
    TransformComponent parent_world{};
    TransformCodecTarget parent_target{.world = &parent_world};
    codec.applyRuntime(decodeOrThrow(codec, parent_json), &parent_target);

    TransformComponent child_world{};
    TransformCodecTarget child_target{.world = &child_world,
                                      .parent_world = &parent_world};
    codec.applyRuntime(decodeOrThrow(codec, local_json), &child_target);
    requireQuat(child_world.rotation,
                glm::normalize(parent_world.rotation * local_rotation));
    REQUIRE(glm::length(child_world.rotation) ==
            Catch::Approx(1.0f).margin(1.0e-6f));
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

TEST_CASE("the accepted key set of every codec is derived from its declared schema",
          "[component-codec][schema][closed-schema][mutation]") {
    // The closed-field list used to be a literal array next to each decoder.
    // It is now generated from the schema the editor reads, so a field that
    // exists in one and not the other cannot survive: no declared field name
    // may be rejected as an unknown document key.
    //
    // The value below is deliberately wrong, but "is it rejected" is not the
    // question and must not be asserted. A decoder that reads the field
    // rejects it for its type or its range; a decoder that ignores the field
    // for the shape it decoded accepts the document outright (collider's
    // half_extents and half_height are unread under the default sphere).
    // Both outcomes are correct here. Only an unknown-key rejection is not,
    // and that is the single thing each section asserts.
    for (const auto &codec : componentCodecs()) {
        for (const auto &field : codec.fieldSchema()) {
            const auto key = wireKeyOf(field.name);
            nlohmann::json authored{{"name", codec.name}};
            authored[key] = nullptr;
            DYNAMIC_SECTION(std::string{codec.name}
                            << "." << std::string{field.name}) {
                std::string rejection;
                try {
                    (void)codec.decodeAuthored(authored);
                } catch (const LegacyRejectException &error) {
                    rejection = error.what();
                    std::transform(rejection.begin(), rejection.end(),
                                   rejection.begin(), [](unsigned char value) {
                                       return static_cast<char>(
                                           std::tolower(value));
                                   });
                }
                INFO("authored key '"
                     << key << "', rejection: "
                     << (rejection.empty() ? std::string{"(accepted)"}
                                           : rejection));
                CHECK(rejection.find("unknown") == std::string::npos);
            }
        }
    }
}

TEST_CASE("transform and light decode and encode through their declared schema",
          "[component-codec][schema][transform][light][mutation]") {
    using Json = nlohmann::json;
    const auto &transform = requireComponentCodec("transform");
    // Omitted fields take the schema defaults, and the canonical key order is
    // the schema's declaration order behind the name envelope.
    const auto minimal =
        canonicalThroughRuntime(transform, Json{{"name", "transform"}});
    CHECK(keysOf(minimal) ==
          std::vector<std::string>{"name", "pos", "rotation", "scale"});
    const auto minimal_json = Json(minimal);
    CHECK(minimal_json.at("pos") == Json::array({0.0, 0.0, 0.0}));
    CHECK(minimal_json.at("rotation") == Json::array({0.0, 0.0, 0.0, 1.0}));
    CHECK(minimal_json.at("scale") == Json::array({1.0, 1.0, 1.0}));
    // The degenerate-rotation rule is the one transform check the schema
    // cannot state, so it is the one that has to be kept by hand.
    CHECK_THROWS(decodeOrThrow(
        transform, Json{{"name", "transform"},
                        {"rotation", Json::array({0, 0, 0, 0})}}));

    const auto &light = requireComponentCodec("light");
    const Json spot{{"name", "light"},      {"type", "spot"},
                    {"position", {0, 0, 0}}, {"direction", {0, -1, 0}},
                    {"color", {1, 1, 1}}};
    const auto canonical = canonicalThroughRuntime(light, spot);
    CHECK(keysOf(canonical) ==
          std::vector<std::string>{"name", "type", "position", "direction",
                                   "intensity", "color", "innerConeAngle",
                                   "outerConeAngle"});
    CHECK(canonical.at("intensity") == 1.0);
    CHECK(canonical.at("innerConeAngle") == 12.5);
    CHECK(canonical.at("outerConeAngle") == 17.5);

    // The range the editor advertises is now the range the decoder enforces.
    auto over_range = spot;
    over_range["intensity"] = 2.0e12;
    CHECK_THROWS(decodeOrThrow(light, over_range));
    auto at_bound = spot;
    at_bound["intensity"] = 1.0e12;
    CHECK_NOTHROW(decodeOrThrow(light, at_bound));

    // Zero is how an omitted range is carried, so it is not an authored value.
    auto zero_range = spot;
    zero_range["range"] = 0.0;
    CHECK_THROWS(decodeOrThrow(light, zero_range));
    auto ranged = spot;
    ranged["range"] = 4.0;
    CHECK(canonicalThroughRuntime(light, ranged).at("range") == 4.0);

    // The type-dependent presence rules survive the move to the schema path.
    auto directional_with_position = spot;
    directional_with_position["type"] = "directional";
    CHECK_THROWS(decodeOrThrow(light, directional_with_position));
    Json point_with_direction{{"name", "light"},
                              {"type", "point"},
                              {"position", {0, 0, 0}},
                              {"direction", {0, -1, 0}},
                              {"color", {1, 1, 1}}};
    CHECK_THROWS(decodeOrThrow(light, point_with_direction));
    Json directional{{"name", "light"},
                     {"type", "directional"},
                     {"direction", {0, -1, 0}},
                     {"color", {1, 1, 1}}};
    auto cone_on_directional = directional;
    cone_on_directional["outerConeAngle"] = 30.0;
    CHECK_THROWS(decodeOrThrow(light, cone_on_directional));
    auto inverted_cone = spot;
    inverted_cone["innerConeAngle"] = 40.0;
    inverted_cone["outerConeAngle"] = 30.0;
    CHECK_THROWS(decodeOrThrow(light, inverted_cone));
    // A directional light carries neither a position nor a cone.
    CHECK(keysOf(canonicalThroughRuntime(light, directional)) ==
          std::vector<std::string>{"name", "type", "direction", "intensity",
                                   "color"});
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

// SC-1: the camera's controller is described by its schema the way the codec
// reads it. Each conditioned field is checked against decodeController itself:
// a type in required_for refuses the field's absence, a type that only applies
// it reads an absent field as the declared default, and no other type keeps it.
TEST_CASE("SC-1 camera controller schema states each type and the fields it reads",
          "[component-codec][camera][controller][schema][sc1]") {
    const auto &codec = requireComponentCodec("camera");
    const auto fields = codec.fieldSchema();
    const auto field = [&](std::string_view name) {
        const auto found = std::find_if(fields.begin(), fields.end(), [&](const auto &entry) {
            return entry.name == name;
        });
        REQUIRE(found != fields.end());
        return *found;
    };
    const auto type = field("controller.type");
    REQUIRE(type.type == StructFieldType::Enum);
    REQUIRE(std::vector<std::string>(type.enum_values, type.enum_values + type.enum_value_count) ==
            std::vector<std::string>{"orbit", "follow", "fly"});

    const auto conditions = componentCodecFieldConditions("camera");
    REQUIRE(conditions.size() == 8);
    REQUIRE(componentCodecFieldConditions("light").empty());
    std::set<std::string> conditioned;
    for (const auto &condition : conditions) {
        (void)field(condition.field);
        REQUIRE(std::string{condition.discriminant} == "controller.type");
        conditioned.insert(std::string{condition.field});
    }
    // Every controller field except the discriminant carries a condition.
    for (const auto &entry : fields) {
        if (!entry.name.starts_with("controller.") || entry.name == "controller.type") continue;
        REQUIRE(conditioned.contains(std::string{entry.name}));
    }
    REQUIRE(field("controller.distance").type == StructFieldType::F32);
    REQUIRE(field("controller.offset").type == StructFieldType::Vec3);
    REQUIRE(field("controller.target").type == StructFieldType::String);
    REQUIRE(std::string{field("controller.yaw").unit} == "rad");

    // The smallest controller each type accepts, and a value for every field.
    const std::map<std::string, nlohmann::json> minimal{
        {"orbit", {{"type", "orbit"}, {"distance", 4.0}}},
        {"follow", {{"type", "follow"}, {"target", "Target"}, {"offset", {0.0, 1.0, 2.0}}}},
        {"fly", {{"type", "fly"}, {"speed", 3.0}, {"sensitivity", 0.5}}},
    };
    const std::map<std::string, nlohmann::json> sample{
        {"target", "Other"}, {"distance", 7.0}, {"yaw", 0.25}, {"pitch", -0.5},
        {"sensitivity", 2.5}, {"offset", {3.0, 4.0, 5.0}}, {"speed", 9.0}, {"damping", 0.75},
    };
    const auto canonical = [&](const nlohmann::json &controller) {
        return nlohmann::json::parse(canonicalThroughRuntime(
            codec, nlohmann::json{{"name", "camera"}, {"controller", controller}}).at("controller").dump());
    };
    const auto accepted = [&](const nlohmann::json &controller) {
        try {
            return codec.decodeAuthored(nlohmann::json{{"name", "camera"}, {"controller", controller}})
                .hasValue();
        } catch (const std::exception &) {
            return false;
        }
    };
    for (const auto &[type_name, base] : minimal) {
        REQUIRE(accepted(base));
        const auto base_canonical = canonical(base);
        for (const auto &condition : conditions) {
            const auto key = std::string{condition.field.substr(std::string_view{"controller."}.size())};
            const bool applies = std::find(condition.applies_to.begin(), condition.applies_to.end(),
                                           type_name) != condition.applies_to.end();
            const bool required = std::find(condition.required_for.begin(), condition.required_for.end(),
                                            type_name) != condition.required_for.end();
            DYNAMIC_SECTION(type_name << "." << key) {
                REQUIRE((!required || applies));
                if (required) {
                    REQUIRE(base.contains(key));
                    auto without = base;
                    without.erase(key);
                    CHECK_FALSE(accepted(without));
                    continue;
                }
                REQUIRE_FALSE(base.contains(key));
                auto with = base;
                with[key] = sample.at(key);
                if (applies) {
                    // An absent field reads as the declared default ...
                    REQUIRE(condition.default_value.has_value());
                    const auto absent = base_canonical.contains(key) ? base_canonical.at(key) : nlohmann::json("");
                    CHECK(absent == nlohmann::json::parse(condition.default_value->dump()));
                    // ... and an authored one is read.
                    CHECK(canonical(with).at(key) == sample.at(key));
                } else if (key != "damping") {
                    // Decoded for every type, but only orbit and follow move
                    // the camera with it (cameracontrollersystem.cpp).
                    CHECK_FALSE(canonical(with).contains(key));
                }
            }
        }
    }
}

} // namespace Pelican


namespace Pelican {
TEST_CASE("componentcodec SV1 uses float decimals and retains authored omissions", "[componentcodec][sv1]") {
    const auto &codec = requireComponentCodec("transform");
    const nlohmann::json authored{{"name", "transform"}, {"pos", {0.1, 0.2, 0.3}}};
    auto decoded = codec.decodeAuthored(authored);
    REQUIRE(decoded);
    const auto encoded = codec.encodeAuthored(decoded.value(), authored);
    CHECK(encoded.at("pos").dump() == "[0.1,0.2,0.3]");
    CHECK_FALSE(encoded.contains("rotation"));
    CHECK_FALSE(encoded.contains("scale"));
    auto explicit_defaults = authored;
    explicit_defaults["scale"] = {1, 1, 1};
    CHECK(codec.encodeAuthored(decoded.value(), explicit_defaults).contains("scale"));
    const auto &camera = requireComponentCodec("camera");
    const nlohmann::json input{{"name", "camera"}, {"type", "perspective"}, {"znear", 0.1},
        {"controller", {{"type", "fly"}, {"speed", 0.3}, {"sensitivity", 0.1}}}};
    const auto camera_value = camera.decodeAuthored(input);
    REQUIRE(camera_value);
    const auto camera_json = camera.encodeAuthored(camera_value.value(), input);
    CHECK(camera_json.at("znear").dump() == "0.1");
    CHECK(camera_json.at("controller").at("speed").dump() == "0.3");
    CHECK_FALSE(camera_json.at("controller").contains("damping"));
}
}

namespace Pelican {
TEST_CASE("componentcodec SV1 numeric writer roundtrips float extremes and double precision", "[componentcodec][sv1]") {
    for (const float value : {0.1f, -0.0f, 3e10f, std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::min(), std::numeric_limits<float>::denorm_min()}) {
        const auto text = renderJsonDocument(internal::authoredJsonNumber(value), {});
        CAPTURE(text);
        const auto reopened = nlohmann::json::parse(text).get<float>();
        CHECK(std::bit_cast<std::uint32_t>(reopened) == std::bit_cast<std::uint32_t>(value));
    }
    CHECK(renderJsonDocument(internal::authoredJsonNumber(3e10f), {}) == "3e+10\n");
    const double precise = 0.12345678901234567;
    CHECK(nlohmann::json::parse(renderJsonDocument(internal::authoredJsonNumber(precise), {})).get<double>() == precise);
}
}

namespace Pelican {
namespace {
struct Sv1BoundedFloats {
    float scalar = 36.8f;
    vec2 vector{-36.8f, 36.8f};
    static constexpr auto schema = structFields(componentPolicy("sv1_bounded"),
        required(field<&Sv1BoundedFloats::scalar>("scalar", frange(-double(36.8f), double(36.8f)))),
        required(field<&Sv1BoundedFloats::vector>("vector", frange(-double(36.8f), double(36.8f)))));
};
}
TEST_CASE("componentcodec SV1 numeric boundaries remain accepted by the unchanged reader", "[componentcodec][sv1]") {
    SECTION("exact F32 limit") {
        const auto &codec = requireComponentCodec("transform");
        const double limit = std::numeric_limits<float>::max();
        const nlohmann::json source{{"name", "transform"}, {"pos", {limit, -limit, 0.1}}};
        auto decoded = codec.decodeAuthored(source);
        REQUIRE(decoded);
        const auto encoded = codec.encodeAuthored(decoded.value(), source);
        const auto reloaded = codec.decodeAuthored(nlohmann::json::parse(renderJsonDocument(encoded, {})));
        REQUIRE(reloaded);
        CHECK(encoded["pos"][0].get<double>() == limit);
        CHECK(encoded["pos"][1].get<double>() == -limit);
    }
    SECTION("declared scalar and vector bounds") {
        const Sv1BoundedFloats original;
        const auto bounded = internal::encodeStructObject(original, Sv1BoundedFloats::schema, "sv1");
        Sv1BoundedFloats reopened;
        internal::decodeStructObject(nlohmann::json(bounded), reopened, Sv1BoundedFloats::schema, "sv1");
        CHECK(reopened.scalar == original.scalar);
        CHECK(reopened.vector.x == original.vector.x);
        CHECK(reopened.vector.y == original.vector.y);
        CHECK(bounded["scalar"].get<double>() == double(36.8f));
    }
}
}
