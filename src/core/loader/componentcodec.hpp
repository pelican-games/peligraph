#pragma once

#include "../cameradefinition.hpp"
#include "../userpublic/details/schema/structfieldschema.hpp"

#include <any>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Pelican {

struct TransformComponent;
struct LocalTransformComponent;

enum class ComponentCodecRuntimeKind : std::uint8_t {
    Ecs,
    Camera,
    Light,
    Collider,
};

enum class ComponentCodecState : std::uint8_t {
    Registered,
    Missing,
};

struct ComponentCodecQueryMetadata {
    ComponentCodecState state = ComponentCodecState::Missing;
    bool editable = false;
    std::string_view codec_name;
};

// Runtime shapes used by the special adapters. They deliberately describe
// runtime values rather than retaining authored JSON.
struct TransformCodecData {
    vec3 pos{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    vec3 scale{1.0f, 1.0f, 1.0f};
};

struct TransformCodecTarget {
    TransformComponent *world = nullptr;
    LocalTransformComponent *local = nullptr;
    const TransformComponent *parent_world = nullptr;
};

struct SimpleModelViewCodecData {
    std::string model;
    std::optional<nlohmann::ordered_json> params;
};

struct CameraCodecData {
    bool projection_specified = false;
    CameraProjectionKind projection_kind = CameraProjectionKind::Perspective;
    float yfov = 0.78539816339f;
    float znear = 0.1f;
    float zfar = 1000.0f;
    std::optional<float> aspect;
    float xmag = 1.0f;
    float ymag = 1.0f;

    bool sprite_specified = false;
    CameraPixelPerfectMode pixel_perfect = CameraPixelPerfectMode::off;
    CameraSpriteSortPolicy sprite_sort = CameraSpriteSortPolicy::z;
    std::optional<nlohmann::ordered_json> controller;
};

enum class LightCodecType : std::uint8_t {
    Directional,
    Point,
    Spot,
};

struct LightCodecData {
    LightCodecType type = LightCodecType::Directional;
    vec3 position{0.0f, 0.0f, 0.0f};
    vec3 direction{0.0f, -1.0f, 0.0f};
    float intensity = 1.0f;
    vec3 color{1.0f, 1.0f, 1.0f}; // authored sRGB; LightContainer projects to linear
    float inner_cone_angle = 12.5f;
    float outer_cone_angle = 17.5f;
    // 0 means the optional authored range is omitted. Authored values must be > 0.
    float range = 0.0f;
};

using ComponentCodecValue = std::any;

// Every registered codec owns exactly these five operations. runtime_apply and
// runtime_project use the target documented by runtime_kind:
//   transform -> TransformCodecTarget, other ECS codecs -> their component,
//   camera/light -> CameraCodecData/LightCodecData, collider -> ColliderComponent.
struct ComponentCodec {
    using DecodeAuthored = ComponentCodecValue (*)(const nlohmann::json &);
    using EncodeCanonical = nlohmann::ordered_json (*)(const ComponentCodecValue &);
    using Schema = std::span<const StructFieldSchema> (*)();
    using RuntimeApply = void (*)(const ComponentCodecValue &, void *);
    using RuntimeProject = ComponentCodecValue (*)(const void *);

    std::string_view name;
    ComponentCodecRuntimeKind runtime_kind;
    DecodeAuthored decode_authored;
    EncodeCanonical encode_canonical;
    Schema schema;
    RuntimeApply runtime_apply;
    RuntimeProject runtime_project;

    ComponentCodecValue decodeAuthored(const nlohmann::json &authored) const;
    nlohmann::ordered_json encodeCanonical(const ComponentCodecValue &value) const;
    std::span<const StructFieldSchema> fieldSchema() const;
    void applyRuntime(const ComponentCodecValue &value, void *target) const;
    ComponentCodecValue projectRuntime(const void *source) const;
};

std::span<const ComponentCodec> componentCodecs();
const ComponentCodec *findComponentCodec(std::string_view name) noexcept;
const ComponentCodec &requireComponentCodec(std::string_view name);
ComponentCodecQueryMetadata componentCodecQueryMetadata(std::string_view name) noexcept;

// Query representation mandated for transform: authored local TRS and the
// display-only world projection are never conflated.
nlohmann::ordered_json projectTransformRuntimeJson(const TransformCodecTarget &target);

} // namespace Pelican
