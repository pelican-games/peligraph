#include "../src/core/ecs/predefined/transform.hpp"
#include "../src/core/loader/componentcodec.hpp"
#include "../src/core/userpublic/components/animation.hpp"
#include "../src/core/userpublic/components/collider.hpp"
#include "../src/core/userpublic/components/localtransform.hpp"
#include "../src/core/userpublic/components/modelview.hpp"
#include "../src/core/userpublic/components/spriteview.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
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

nlohmann::ordered_json canonicalThroughRuntime(const ComponentCodec &codec,
                                               const nlohmann::json &authored) {
    const auto decoded = codec.decodeAuthored(authored);
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

TEST_CASE("component codec registry exposes the complete five-tuple", "[component-codec][wp151]") {
    const auto codecs = componentCodecs();
    REQUIRE(codecs.size() == 7);
    std::unordered_set<std::string_view> names;
    for (const auto &codec : codecs) {
        REQUIRE(names.insert(codec.name).second);
        REQUIRE(codec.decode_authored != nullptr);
        REQUIRE(codec.encode_canonical != nullptr);
        REQUIRE(codec.schema != nullptr);
        REQUIRE(codec.runtime_apply != nullptr);
        REQUIRE(codec.runtime_project != nullptr);
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

TEST_CASE("seven component codecs roundtrip authored runtime canonical and fresh load",
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
    codec.applyRuntime(codec.decodeAuthored(parent_json), &parent_target);

    TransformComponent child_world{};
    LocalTransformComponent child_local{};
    TransformCodecTarget child_target{
        .world = &child_world, .local = &child_local, .parent_world = &parent_world};
    codec.applyRuntime(codec.decodeAuthored(child_json), &child_target);

    requireVec3(child_world.pos, {4.0f, 2.0f, 12.0f});
    requireVec3(child_world.scale, {1.0f, 6.0f, 4.0f});
    const auto runtime_json = projectTransformRuntimeJson(child_target);
    REQUIRE(runtime_json.at("local_trs") == codec.encodeCanonical(codec.decodeAuthored(child_json)));
    const auto &world_pos = runtime_json.at("world_trs").at("pos");
    REQUIRE(world_pos.at(0).get<float>() == Catch::Approx(4.0f).margin(1.0e-5f));
    REQUIRE(world_pos.at(1).get<float>() == Catch::Approx(2.0f).margin(1.0e-5f));
    REQUIRE(world_pos.at(2).get<float>() == Catch::Approx(12.0f).margin(1.0e-5f));

    TransformCodecTarget inverse_target{.world = &child_world, .parent_world = &parent_world};
    const auto inverse_local = codec.encodeCanonical(codec.projectRuntime(&inverse_target));
    const auto expected_local = codec.encodeCanonical(codec.decodeAuthored(child_json));
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
    codec.applyRuntime(codec.decodeAuthored(runtime_json.at("local_trs")), &fresh_target);
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

TEST_CASE("every component codec rejects invalid type range and unknown key fixtures",
          "[component-codec][invalid][wp151]") {
    std::unordered_set<std::string> covered;
    for (const auto &entry : readFixture("invalid.json")) {
        const auto name = entry.at("name").get<std::string>();
        const auto kind = entry.at("kind").get<std::string>();
        DYNAMIC_SECTION(name << "/" << kind) {
            REQUIRE_THROWS(requireComponentCodec(name).decodeAuthored(entry.at("authored")));
        }
        covered.insert(name + "/" + kind);
    }
    for (const auto &codec : componentCodecs()) {
        for (const auto kind : {"type", "range", "unknown"}) {
            REQUIRE(covered.contains(std::string{codec.name} + "/" + kind));
        }
    }
}

} // namespace Pelican
