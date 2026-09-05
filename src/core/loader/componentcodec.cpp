#include "componentcodec.hpp"
#include "../renderer/proceduralgeometrycomponent.hpp"

#include "../ecs/core.hpp"
#include "../ecs/predefined/transform.hpp"
#include "../geomhelper/geomhelper.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../shader/raygrouplayout.hpp"
#include "../userpublic/components/animation.hpp"
#include "../userpublic/components/collider.hpp"
#include "../userpublic/components/localtransform.hpp"
#include "../userpublic/components/modelview.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../userpublic/components/rayinstance.hpp"
#include "../userpublic/components/spriteview.hpp"
#include "../userpublic/details/schema/structfieldjson.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

thread_local ComponentCodecCanonicalDefaults codec_canonical_defaults;
thread_local const std::vector<ComponentCodec> *codec_catalog_override =
    nullptr;

class DecodedUserDataViolation final {
    UserDataViolation violation_;

  public:
    explicit DecodedUserDataViolation(UserDataViolation violation)
        : violation_{std::move(violation)} {}

    UserDataViolation take() { return std::move(violation_); }
};

[[noreturn]] void rejectDecodedUserData(UserDataViolation violation) {
    throw DecodedUserDataViolation{std::move(violation)};
}

template <ComponentCodecValue (*Decode)(const Json &)>
ResolutionResult<ComponentCodecValue> decodeResult(const Json &authored) {
    try {
        return ResolutionResult<ComponentCodecValue>::success(
            Decode(authored));
    } catch (DecodedUserDataViolation &error) {
        return ResolutionResult<ComponentCodecValue>::failure(error.take());
    }
}

enum class ColliderSchemaShape : std::uint8_t { Sphere, Box, Capsule };

struct CameraSchemaFields {
    CameraProjectionKind type = CameraProjectionKind::Perspective;
    float yfov = 0.78539816339f;
    float znear = 0.1f;
    float zfar = 1000.0f;
    float aspect = 1.0f;
    float xmag = 1.0f;
    float ymag = 1.0f;
    CameraPixelPerfectMode pixel_perfect = CameraPixelPerfectMode::off;
    CameraSpriteSortPolicy sort = CameraSpriteSortPolicy::z;
    std::string controller_type;
};

struct ColliderSchemaFields {
    ColliderSchemaShape shape = ColliderSchemaShape::Sphere;
    vec3 pos{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    float radius = 0.5f;
    vec3 half_extents{0.5f, 0.5f, 0.5f};
    float half_height = 0.5f;
    std::uint32_t layer = 1;
    std::uint32_t mask = ~std::uint32_t{0};
    bool trigger = false;
    bool one_way = false;
};

struct AnimationSchemaFields {
    std::string clip;
    double speed = 1.0;
    bool loop = true;
    double start_time = 0.0;
};

struct SpriteSchemaFields {
    std::string texture;
    vec2 size{0.0f, 0.0f};
    vec2 pivot{0.5f, 0.5f};
    vec4 color{1.0f, 1.0f, 1.0f, 1.0f};
    bool flip_x = false;
    bool flip_y = false;
    std::int16_t layer = 0;
    SpriteBillboard billboard = SpriteBillboard::none;
};

inline constexpr auto transform_schema = structFields(
    componentPolicy("transform"),
    defaulted(field<&TransformCodecData::pos>("pos"), vec3{0.0f, 0.0f, 0.0f}),
    defaulted(field<&TransformCodecData::rotation>("rotation"), quat{0.0f, 0.0f, 0.0f, 1.0f}),
    defaulted(field<&TransformCodecData::scale>("scale"), vec3{1.0f, 1.0f, 1.0f}));

inline constexpr auto simple_model_view_schema = structFields(
    componentPolicy("simplemodelview"),
    required(field<&SimpleModelViewCodecData::model>("model")));

inline constexpr auto procedural_geometry_schema = structFields(
    componentPolicy("pelican_procedural_geometry"),
    required(field<&ProceduralGeometryComponent::asset>("asset")));

inline constexpr auto camera_schema = structFields(
    componentPolicy("camera"),
    defaulted(field<&CameraSchemaFields::type>("type"), CameraProjectionKind::Perspective,
              enumValues(enumValue("perspective", CameraProjectionKind::Perspective),
                         enumValue("orthographic", CameraProjectionKind::Orthographic))),
    defaulted(field<&CameraSchemaFields::yfov>("yfov", frange(0.0, 3.14159265358979323846), "rad"),
              0.78539816339f),
    defaulted(field<&CameraSchemaFields::znear>("znear", frange(0.0, 1.0e12)), 0.1f),
    defaulted(field<&CameraSchemaFields::zfar>("zfar", frange(0.0, 1.0e12)), 1000.0f),
    defaulted(field<&CameraSchemaFields::aspect>("aspect", frange(0.0, 1.0e6)), 1.0f),
    defaulted(field<&CameraSchemaFields::xmag>("xmag", frange(0.0, 1.0e12)), 1.0f),
    defaulted(field<&CameraSchemaFields::ymag>("ymag", frange(0.0, 1.0e12)), 1.0f),
    defaulted(field<&CameraSchemaFields::pixel_perfect>("sprite.pixel_perfect"), CameraPixelPerfectMode::off,
              enumValues(enumValue("off", CameraPixelPerfectMode::off),
                         enumValue("strict", CameraPixelPerfectMode::strict))),
    defaulted(field<&CameraSchemaFields::sort>("sprite.sort"), CameraSpriteSortPolicy::z,
              enumValues(enumValue("z", CameraSpriteSortPolicy::z),
                         enumValue("y_down", CameraSpriteSortPolicy::y_down),
                         enumValue("declaration", CameraSpriteSortPolicy::declaration))),
    defaulted(field<&CameraSchemaFields::controller_type>("controller.type"), ""));

inline constexpr auto light_schema = structFields(
    componentPolicy("light"),
    defaulted(field<&LightCodecData::type>("type"), LightCodecType::Directional,
              enumValues(enumValue("directional", LightCodecType::Directional),
                         enumValue("point", LightCodecType::Point),
                         enumValue("spot", LightCodecType::Spot))),
    defaulted(field<&LightCodecData::position>("position"), vec3{0.0f, 0.0f, 0.0f}),
    defaulted(field<&LightCodecData::direction>("direction"), vec3{0.0f, -1.0f, 0.0f}),
    defaulted(field<&LightCodecData::intensity>("intensity", frange(0.0, 1.0e12)), 1.0f),
    defaulted(field<&LightCodecData::range>("range", frange(0.0, 1.0e12)), 0.0f),
    defaulted(field<&LightCodecData::color>("color", frange(0.0, 1.0)), vec3{1.0f, 1.0f, 1.0f}),
    defaulted(field<&LightCodecData::inner_cone_angle>("innerConeAngle", frange(0.0, 180.0), "degree"),
              12.5f),
    defaulted(field<&LightCodecData::outer_cone_angle>("outerConeAngle", frange(0.0, 180.0), "degree"),
              17.5f));

inline constexpr auto collider_schema = structFields(
    componentPolicy("collider"),
    defaulted(field<&ColliderSchemaFields::shape>("shape"), ColliderSchemaShape::Sphere,
              enumValues(enumValue("sphere", ColliderSchemaShape::Sphere),
                         enumValue("box", ColliderSchemaShape::Box),
                         enumValue("capsule", ColliderSchemaShape::Capsule))),
    defaulted(field<&ColliderSchemaFields::pos>("pos"), vec3{0.0f, 0.0f, 0.0f}),
    defaulted(field<&ColliderSchemaFields::rotation>("rotation"), quat{0.0f, 0.0f, 0.0f, 1.0f}),
    defaulted(field<&ColliderSchemaFields::radius>("radius", frange(0.0, 1.0e12)), 0.5f),
    defaulted(field<&ColliderSchemaFields::half_extents>("half_extents", frange(0.0, 1.0e12)),
              vec3{0.5f, 0.5f, 0.5f}),
    defaulted(field<&ColliderSchemaFields::half_height>("half_height", frange(0.0, 1.0e12)), 0.5f),
    defaulted(field<&ColliderSchemaFields::layer>("layer"), std::uint32_t{1}),
    defaulted(field<&ColliderSchemaFields::mask>("mask"), ~std::uint32_t{0}),
    defaulted(field<&ColliderSchemaFields::trigger>("trigger"), false),
    defaulted(field<&ColliderSchemaFields::one_way>("one_way"), false));

inline constexpr auto animation_schema = structFields(
    componentPolicy("animation"),
    required(field<&AnimationSchemaFields::clip>("clip")),
    defaulted(field<&AnimationSchemaFields::speed>("speed", frange(0.0, 1.0e12)), 1.0),
    defaulted(field<&AnimationSchemaFields::loop>("loop"), true),
    defaulted(field<&AnimationSchemaFields::start_time>("start_time", frange(0.0, 1.0e12), "second"), 0.0));

inline constexpr auto sprite_schema = structFields(
    componentPolicy("sprite_view"),
    required(field<&SpriteSchemaFields::texture>("texture")),
    defaulted(field<&SpriteSchemaFields::size>("size", frange(0.0, 1.0e12)), vec2{0.0f, 0.0f}),
    defaulted(field<&SpriteSchemaFields::pivot>("pivot", frange(0.0, 1.0)), vec2{0.5f, 0.5f}),
    defaulted(field<&SpriteSchemaFields::color>("color"), vec4{1.0f, 1.0f, 1.0f, 1.0f}),
    defaulted(field<&SpriteSchemaFields::flip_x>("flip[0]"), false),
    defaulted(field<&SpriteSchemaFields::flip_y>("flip[1]"), false),
    defaulted(field<&SpriteSchemaFields::layer>("layer"), std::int16_t{0}),
    defaulted(field<&SpriteSchemaFields::billboard>("billboard"), SpriteBillboard::none,
              enumValues(enumValue("none", SpriteBillboard::none),
                         enumValue("y_axis", SpriteBillboard::y_axis),
                         enumValue("full", SpriteBillboard::full))));

template <class Schema, std::size_t... Index>
auto materializeSchema(const Schema &schema, std::index_sequence<Index...>) {
    return std::array{internal::materializeStructField(schema.fields[Index])...};
}

template <class Schema>
auto materializeSchema(const Schema &schema) {
    using Fields = std::remove_cvref_t<decltype(schema.fields)>;
    return materializeSchema(schema, std::make_index_sequence<std::tuple_size_v<Fields>>{});
}

const auto transform_fields = materializeSchema(transform_schema);
const auto simple_model_view_fields = materializeSchema(simple_model_view_schema);
const auto procedural_geometry_fields =
    materializeSchema(procedural_geometry_schema);
const auto camera_fields = materializeSchema(camera_schema);
const auto light_fields = materializeSchema(light_schema);
const auto collider_fields = materializeSchema(collider_schema);
const auto animation_fields = materializeSchema(animation_schema);
const auto sprite_fields = materializeSchema(sprite_schema);

const std::array ray_instance_fields{
    StructFieldSchema{"mask", StructFieldType::U8,
                      std::pair<std::uint64_t, std::uint64_t>{0, 0xffu}},
    StructFieldSchema{"instance_flags", StructFieldType::Enum,
                      std::monostate{}},
    StructFieldSchema{"custom_index", StructFieldType::U32,
                      std::pair<std::uint64_t, std::uint64_t>{
                          0, rayCustomIndexMaximum}},
};

[[noreturn]] void codecError(StructFieldErrorCode code, std::string path, std::string_view detail) {
    internal::structFieldError(code, std::move(path), detail);
}

void requireObjectAndName(const Json &json, std::string_view component_name) {
    if (!json.is_object()) {
        codecError(StructFieldErrorCode::PayloadMustBeObject, std::string{component_name}, "must be an object");
    }
    const auto name = json.find("name");
    if (name == json.end()) {
        codecError(StructFieldErrorCode::MissingField, std::string{component_name} + ".name", "is required");
    }
    if (!name->is_string() || name->get_ref<const std::string &>() != component_name) {
        codecError(StructFieldErrorCode::TypeMismatch, std::string{component_name} + ".name",
                   "must match the codec name");
    }
}

template <std::size_t N>
void requireClosed(const Json &json, std::string_view root, const std::array<std::string_view, N> &known) {
    for (const auto &[key, value] : json.items()) {
        (void)value;
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            codecError(StructFieldErrorCode::UnknownField, std::string{root} + "." + key, "is unknown");
        }
    }
}

const Json &requireField(const Json &json, std::string_view root, std::string_view field) {
    const auto found = json.find(field);
    if (found == json.end()) {
        codecError(StructFieldErrorCode::MissingField, std::string{root} + "." + std::string{field},
                   "is required");
    }
    return *found;
}

std::string readString(const Json &value, std::string path, bool non_empty = false) {
    if (!value.is_string()) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be a string");
    }
    auto result = value.get<std::string>();
    if (non_empty && result.empty()) {
        codecError(StructFieldErrorCode::OutOfRange, std::move(path), "must not be empty");
    }
    return result;
}

float readFloat(const Json &value, std::string path,
                float min = -std::numeric_limits<float>::max(),
                float max = std::numeric_limits<float>::max(), bool min_inclusive = true) {
    if (!value.is_number()) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be a number");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number) || number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        number > static_cast<double>(std::numeric_limits<float>::max())) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be finite F32");
    }
    const bool below = min_inclusive ? number < min : number <= min;
    if (below || number > max) {
        codecError(StructFieldErrorCode::OutOfRange, std::move(path), "is out of range");
    }
    return static_cast<float>(number);
}

double readDouble(const Json &value, std::string path, double min, bool min_inclusive = true) {
    if (!value.is_number()) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be a number");
    }
    const auto number = value.get<double>();
    if (!std::isfinite(number)) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be finite");
    }
    const bool below = min_inclusive ? number < min : number <= min;
    if (below) {
        codecError(StructFieldErrorCode::OutOfRange, std::move(path), "is out of range");
    }
    return number;
}

vec2 readVec2(const Json &value, std::string path) {
    if (!value.is_array() || value.size() != 2) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be a vec2 array");
    }
    return {readFloat(value[0], path), readFloat(value[1], path)};
}

vec3 readVec3(const Json &value, std::string path, float min = -std::numeric_limits<float>::max(),
              float max = std::numeric_limits<float>::max(), bool min_inclusive = true) {
    if (!value.is_array() || value.size() != 3) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be a vec3 array");
    }
    return {readFloat(value[0], path, min, max, min_inclusive),
            readFloat(value[1], path, min, max, min_inclusive),
            readFloat(value[2], path, min, max, min_inclusive)};
}

vec4 readVec4(const Json &value, std::string path) {
    if (!value.is_array() || value.size() != 4) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be a vec4 array");
    }
    return {readFloat(value[0], path), readFloat(value[1], path),
            readFloat(value[2], path), readFloat(value[3], path)};
}

quat readQuat(const Json &value, std::string path) {
    const auto v = readVec4(value, path);
    const auto norm_squared = v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w;
    if (norm_squared <= std::numeric_limits<float>::epsilon()) {
        codecError(StructFieldErrorCode::OutOfRange, std::move(path), "must be a non-zero quaternion");
    }
    return {v.x, v.y, v.z, v.w};
}

bool readBool(const Json &value, std::string path) {
    if (!value.is_boolean()) {
        codecError(StructFieldErrorCode::TypeMismatch, std::move(path), "must be a boolean");
    }
    return value.get<bool>();
}

template <class T> const T &codecValue(const ComponentCodecValue &value, std::string_view codec) {
    const auto *typed = std::any_cast<T>(&value);
    if (typed == nullptr) {
        throw std::invalid_argument("component codec value type mismatch for " + std::string{codec});
    }
    return *typed;
}

OrderedJson encodeVec(vec2 value) { return OrderedJson::array({value.x, value.y}); }
OrderedJson encodeVec(vec3 value) { return OrderedJson::array({value.x, value.y, value.z}); }
OrderedJson encodeVec(vec4 value) { return OrderedJson::array({value.x, value.y, value.z, value.w}); }
OrderedJson encodeVec(quat value) { return OrderedJson::array({value.x, value.y, value.z, value.w}); }

ComponentCodecValue decodeTransform(const Json &json) {
    requireObjectAndName(json, "transform");
    requireClosed(json, "transform", std::array<std::string_view, 4>{"name", "pos", "rotation", "scale"});
    TransformCodecData result;
    if (const auto it = json.find("pos"); it != json.end()) result.pos = readVec3(*it, "transform.pos");
    if (const auto it = json.find("rotation"); it != json.end()) {
        result.rotation = readQuat(*it, "transform.rotation");
        result.rotation_specified = true;
    }
    if (const auto it = json.find("scale"); it != json.end()) result.scale = readVec3(*it, "transform.scale");
    return result;
}

OrderedJson encodeTransform(const ComponentCodecValue &value) {
    const auto &data = codecValue<TransformCodecData>(value, "transform");
    return OrderedJson{{"name", "transform"}, {"pos", encodeVec(data.pos)},
                       {"rotation", encodeVec(data.rotation)}, {"scale", encodeVec(data.scale)}};
}

void applyTransform(const ComponentCodecValue &value, void *raw_target) {
    if (raw_target == nullptr) throw std::invalid_argument("transform runtime target is null");
    const auto &data = codecValue<TransformCodecData>(value, "transform");
    auto &target = *static_cast<TransformCodecTarget *>(raw_target);
    if (target.world == nullptr) throw std::invalid_argument("transform world target is null");

    if (target.local != nullptr) {
        target.local->pos = data.pos;
        target.local->rotation = data.rotation;
        target.local->scale = data.scale;
    }

    glm::vec3 parent_pos{0.0f};
    glm::quat parent_rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 parent_scale{1.0f};
    if (target.parent_world != nullptr) {
        parent_pos = target.parent_world->pos;
        parent_rotation = target.parent_world->rotation;
        parent_scale = target.parent_world->scale;
    }
    target.world->scale = parent_scale * to_glm(data.scale);
    target.world->rotation = parent_rotation * to_glm(data.rotation);
    target.world->pos = parent_pos + parent_rotation * (parent_scale * to_glm(data.pos));
}

ComponentCodecValue projectTransform(const void *raw_source) {
    if (raw_source == nullptr) throw std::invalid_argument("transform runtime source is null");
    const auto &source = *static_cast<const TransformCodecTarget *>(raw_source);
    if (source.local != nullptr) {
        return TransformCodecData{source.local->pos, source.local->rotation,
                                  source.local->scale, true};
    }
    if (source.world == nullptr) throw std::invalid_argument("transform world source is null");

    if (source.parent_world != nullptr) {
        const auto &parent = *source.parent_world;
        constexpr float invertible_epsilon = std::numeric_limits<float>::epsilon();
        if (std::abs(parent.scale.x) <= invertible_epsilon ||
            std::abs(parent.scale.y) <= invertible_epsilon ||
            std::abs(parent.scale.z) <= invertible_epsilon) {
            codecError(StructFieldErrorCode::OutOfRange, "transform.parent.scale",
                       "cannot invert a zero parent scale");
        }
        const auto parent_rotation_inverse = glm::inverse(parent.rotation);
        const auto local_pos =
            (parent_rotation_inverse * (source.world->pos - parent.pos)) / parent.scale;
        const auto local_rotation = parent_rotation_inverse * source.world->rotation;
        const auto local_scale = source.world->scale / parent.scale;
        return TransformCodecData{{local_pos.x, local_pos.y, local_pos.z},
                                  {local_rotation.x, local_rotation.y,
                                   local_rotation.z, local_rotation.w},
                                  {local_scale.x, local_scale.y, local_scale.z}, true};
    }
    return TransformCodecData{{source.world->pos.x, source.world->pos.y, source.world->pos.z},
                              {source.world->rotation.x, source.world->rotation.y,
                               source.world->rotation.z, source.world->rotation.w},
                              {source.world->scale.x, source.world->scale.y, source.world->scale.z}, true};
}

const Json *optionalObject(const Json &json, std::string_view root, std::string_view field);

ComponentCodecValue decodeSimpleModelView(const Json &json) {
    requireObjectAndName(json, "simplemodelview");
    requireClosed(json, "simplemodelview", std::array<std::string_view, 3>{"name", "model", "params"});
    SimpleModelViewCodecData result;
    result.model = readString(requireField(json, "simplemodelview", "model"), "simplemodelview.model", true);
    if (const auto *params = optionalObject(json, "simplemodelview", "params")) {
        result.params = *params;
    }
    return result;
}

OrderedJson encodeSimpleModelView(const ComponentCodecValue &value) {
    const auto &data = codecValue<SimpleModelViewCodecData>(value, "simplemodelview");
    if (data.model.empty()) codecError(StructFieldErrorCode::OutOfRange, "simplemodelview.model", "must not be empty");
    OrderedJson result{{"name", "simplemodelview"}, {"model", data.model}};
    if (data.params) result["params"] = *data.params;
    return result;
}

void applySimpleModelView(const ComponentCodecValue &value, void *raw_target) {
    if (raw_target == nullptr) throw std::invalid_argument("simplemodelview runtime target is null");
    const auto &data = codecValue<SimpleModelViewCodecData>(value, "simplemodelview");
    auto &target = *static_cast<SimpleModelViewComponent *>(raw_target);
    if (target.model_name == data.model) return;
    if (target.model_instance_id) {
        GET_MODULE(PolygonInstanceContainer).removeModelInstance(*target.model_instance_id);
        target.model_instance_id.reset();
    }
    target.model_name = data.model;
    target.dirty = 1;
}

ComponentCodecValue projectSimpleModelView(const void *raw_source) {
    if (raw_source == nullptr) throw std::invalid_argument("simplemodelview runtime source is null");
    return SimpleModelViewCodecData{
        .model = static_cast<const SimpleModelViewComponent *>(raw_source)->model_name};
}

ComponentCodecValue decodeProceduralGeometry(const Json &json) {
    requireObjectAndName(json, "pelican_procedural_geometry");
    requireClosed(json, "pelican_procedural_geometry",
                  std::array<std::string_view, 2>{"name", "asset"});
    ProceduralGeometryComponent result;
    result.asset = readString(
        requireField(json, "pelican_procedural_geometry", "asset"),
        "pelican_procedural_geometry.asset", true);
    return result;
}

OrderedJson encodeProceduralGeometry(const ComponentCodecValue &value) {
    const auto &data = codecValue<ProceduralGeometryComponent>(
        value, "pelican_procedural_geometry");
    if (data.asset.empty()) {
        codecError(StructFieldErrorCode::OutOfRange,
                   "pelican_procedural_geometry.asset",
                   "must not be empty");
    }
    return OrderedJson{{"name", "pelican_procedural_geometry"},
                       {"asset", data.asset}};
}

void applyProceduralGeometry(const ComponentCodecValue &value,
                             void *raw_target) {
    if (raw_target == nullptr) {
        throw std::invalid_argument(
            "pelican_procedural_geometry runtime target is null");
    }
    *static_cast<ProceduralGeometryComponent *>(raw_target) =
        codecValue<ProceduralGeometryComponent>(
            value, "pelican_procedural_geometry");
}

ComponentCodecValue projectProceduralGeometry(const void *raw_source) {
    if (raw_source == nullptr) {
        throw std::invalid_argument(
            "pelican_procedural_geometry runtime source is null");
    }
    return *static_cast<const ProceduralGeometryComponent *>(raw_source);
}

const Json *optionalObject(const Json &json, std::string_view root, std::string_view field) {
    const auto it = json.find(field);
    if (it == json.end()) return nullptr;
    if (!it->is_object()) {
        codecError(StructFieldErrorCode::TypeMismatch, std::string{root} + "." + std::string{field},
                   "must be an object");
    }
    return &*it;
}

const Json *findCameraNumber(const Json &json, const Json *nested, std::string_view name) {
    if (nested != nullptr) {
        if (const auto it = nested->find(name); it != nested->end()) return &*it;
    }
    if (const auto it = json.find(name); it != json.end()) return &*it;
    return nullptr;
}

CameraControllerCodecData decodeController(const Json &controller) {
    requireClosed(controller, "camera.controller",
                  std::array<std::string_view, 12>{"type", "target", "offset", "distance", "yaw", "pitch",
                                                   "yaw_degrees", "pitch_degrees", "damping", "speed",
                                                   "sensitivity", "up"});
    const auto type = readString(requireField(controller, "camera.controller", "type"),
                                 "camera.controller.type", true);
    CameraControllerCodecData result;
    result.damping = controller.contains("damping")
                         ? readFloat(controller.at("damping"), "camera.controller.damping", 0.0f)
                         : 0.0f;
    constexpr float degrees_to_radians = 0.01745329251994329577f;
    if (type == "orbit") {
        result.type = CameraControllerCodecType::Orbit;
        if (controller.contains("target")) {
            result.target = readString(controller.at("target"),
                                       "camera.controller.target", true);
        }
        result.distance = readFloat(requireField(controller, "camera.controller", "distance"),
                                    "camera.controller.distance", 0.0f,
                                    std::numeric_limits<float>::max(), false);
        const auto read_angle = [&](std::string_view radians, std::string_view degrees) {
            if (controller.contains(radians) && controller.contains(degrees)) {
                codecError(StructFieldErrorCode::UnknownField,
                           "camera.controller." + std::string{degrees}, "duplicates its radian field");
            }
            if (controller.contains(radians)) {
                return readFloat(controller.at(radians), "camera.controller." + std::string{radians});
            }
            if (controller.contains(degrees)) {
                return readFloat(controller.at(degrees), "camera.controller." + std::string{degrees}) *
                       degrees_to_radians;
            }
            return 0.0f;
        };
        result.yaw = read_angle("yaw", "yaw_degrees");
        result.pitch = read_angle("pitch", "pitch_degrees");
        result.sensitivity = controller.contains("sensitivity")
                                 ? readFloat(controller.at("sensitivity"), "camera.controller.sensitivity", 0.0f)
                                 : 1.0f;
    } else if (type == "follow") {
        result.type = CameraControllerCodecType::Follow;
        result.target = readString(requireField(controller, "camera.controller", "target"),
                                   "camera.controller.target", true);
        result.offset = readVec3(requireField(controller, "camera.controller", "offset"),
                                 "camera.controller.offset");
    } else if (type == "fly") {
        result.type = CameraControllerCodecType::Fly;
        result.speed = readFloat(requireField(controller, "camera.controller", "speed"),
                                 "camera.controller.speed", 0.0f,
                                 std::numeric_limits<float>::max(), false);
        result.sensitivity = readFloat(requireField(controller, "camera.controller", "sensitivity"),
                                       "camera.controller.sensitivity", 0.0f,
                                       std::numeric_limits<float>::max(), false);
    } else {
        codecError(StructFieldErrorCode::InvalidEnum, "camera.controller.type",
                   "value '" + type + "' is not supported");
    }
    return result;
}

ComponentCodecValue decodeCamera(const Json &json) {
    requireObjectAndName(json, "camera");
    const auto reject_aliases = [](const Json &object, std::string_view root) {
        constexpr std::array aliases{
            std::pair<std::string_view, std::string_view>{"fov_y", "yfov"},
            std::pair<std::string_view, std::string_view>{"near", "znear"},
            std::pair<std::string_view, std::string_view>{"far", "zfar"},
        };
        for (const auto &[alias, replacement] : aliases) {
            if (object.contains(alias)) {
                rejectDecodedUserData(UserDataViolation{
                    .code = "camera.field_alias_not_supported@1",
                    .tokens = {std::string{alias}},
                    .fields = OrderedJson{{"alias", alias},
                                          {"replacement", replacement}},
                    .leaf_message =
                        std::string{root} + " field '" + std::string{alias} +
                        "' is not supported in v1; use '" +
                        std::string{replacement} +
                        (alias == "fov_y" ? "' (radians)" : "'"),
                });
            }
        }
    };
    reject_aliases(json, "camera");
    requireClosed(json, "camera",
                  std::array<std::string_view, 13>{"name", "type", "perspective", "orthographic", "yfov",
                                                   "znear", "zfar", "aspect", "xmag", "ymag", "sprite",
                                                   "controller", "params"});
    const auto *perspective = optionalObject(json, "camera", "perspective");
    const auto *orthographic = optionalObject(json, "camera", "orthographic");
    if (perspective != nullptr && orthographic != nullptr) {
        codecError(StructFieldErrorCode::UnknownField, "camera.orthographic",
                   "cannot be combined with perspective");
    }
    if (perspective != nullptr) {
        reject_aliases(*perspective, "camera.perspective");
        requireClosed(*perspective, "camera.perspective",
                      std::array<std::string_view, 4>{"yfov", "znear", "zfar", "aspect"});
    }
    if (orthographic != nullptr) {
        reject_aliases(*orthographic, "camera.orthographic");
        requireClosed(*orthographic, "camera.orthographic",
                      std::array<std::string_view, 4>{"xmag", "ymag", "znear", "zfar"});
    }

    CameraCodecData result;
    constexpr std::array projection_fields{"type", "yfov", "znear", "zfar", "aspect", "xmag", "ymag"};
    result.projection_specified = perspective != nullptr || orthographic != nullptr ||
                                  std::any_of(projection_fields.begin(), projection_fields.end(),
                                              [&](auto field) { return json.contains(field); });
    std::string type;
    if (json.contains("type")) type = readString(json.at("type"), "camera.type");
    if (type.empty()) type = orthographic != nullptr || json.contains("xmag") || json.contains("ymag")
                                 ? "orthographic"
                                 : "perspective";
    if (type == "perspective") result.projection_kind = CameraProjectionKind::Perspective;
    else if (type == "orthographic") result.projection_kind = CameraProjectionKind::Orthographic;
    else codecError(StructFieldErrorCode::InvalidEnum, "camera.type", "must be perspective or orthographic");

    const auto *nested = result.projection_kind == CameraProjectionKind::Perspective ? perspective : orthographic;
    if (result.projection_kind == CameraProjectionKind::Perspective) {
        if (const auto *value = findCameraNumber(json, nested, "yfov")) {
            result.yfov = readFloat(*value, "camera.yfov", 0.0f, 3.14159265358979323846f, false);
        }
        if (const auto *value = findCameraNumber(json, nested, "aspect")) {
            result.aspect = readFloat(*value, "camera.aspect", 0.0f,
                                      std::numeric_limits<float>::max(), false);
        }
    } else {
        if (const auto *value = findCameraNumber(json, nested, "xmag")) {
            result.xmag = readFloat(*value, "camera.xmag", 0.0f,
                                    std::numeric_limits<float>::max(), false);
        }
        if (const auto *value = findCameraNumber(json, nested, "ymag")) {
            result.ymag = readFloat(*value, "camera.ymag", 0.0f,
                                    std::numeric_limits<float>::max(), false);
        }
    }
    if (const auto *value = findCameraNumber(json, nested, "znear")) {
        result.znear = readFloat(*value, "camera.znear", 0.0f,
                                 std::numeric_limits<float>::max(), false);
    }
    if (const auto *value = findCameraNumber(json, nested, "zfar")) {
        result.zfar = readFloat(*value, "camera.zfar", 0.0f,
                                std::numeric_limits<float>::max(), false);
    }
    if (result.znear && result.zfar) {
        if (auto violation =
                cameraCrossFieldViolation(*result.znear, *result.zfar)) {
            rejectDecodedUserData(std::move(*violation));
        }
    }

    if (const auto *sprite = optionalObject(json, "camera", "sprite")) {
        requireClosed(*sprite, "camera.sprite",
                      std::array<std::string_view, 2>{"pixel_perfect", "sort"});
        result.sprite_specified = true;
        const auto pixel = sprite->contains("pixel_perfect")
                               ? readString(sprite->at("pixel_perfect"),
                                            "camera.sprite.pixel_perfect")
                               : std::string{"off"};
        if (pixel == "off") result.pixel_perfect = CameraPixelPerfectMode::off;
        else if (pixel == "strict") result.pixel_perfect = CameraPixelPerfectMode::strict;
        else codecError(StructFieldErrorCode::InvalidEnum, "camera.sprite.pixel_perfect", "is invalid");
        const auto sort = sprite->contains("sort")
                              ? readString(sprite->at("sort"),
                                           "camera.sprite.sort")
                              : std::string{"z"};
        if (sort == "z") result.sprite_sort = CameraSpriteSortPolicy::z;
        else if (sort == "y_down") result.sprite_sort = CameraSpriteSortPolicy::y_down;
        else if (sort == "declaration") result.sprite_sort = CameraSpriteSortPolicy::declaration;
        else codecError(StructFieldErrorCode::InvalidEnum, "camera.sprite.sort", "is invalid");
    }

    const Json *controller = optionalObject(json, "camera", "controller");
    if (const auto *params = optionalObject(json, "camera", "params")) {
        requireClosed(*params, "camera.params", std::array<std::string_view, 1>{"controller"});
        const auto *nested_controller = optionalObject(*params, "camera.params", "controller");
        if (controller != nullptr && nested_controller != nullptr) {
            codecError(StructFieldErrorCode::UnknownField, "camera.params.controller",
                       "duplicates camera.controller");
        }
        if (nested_controller != nullptr) controller = nested_controller;
    }
    if (controller != nullptr) result.controller = decodeController(*controller);
    return result;
}

std::string_view cameraTypeName(CameraProjectionKind kind) {
    return kind == CameraProjectionKind::Perspective ? "perspective" : "orthographic";
}

OrderedJson encodeController(const CameraControllerCodecData &data) {
    OrderedJson result;
    result["type"] = data.type == CameraControllerCodecType::Orbit
                         ? "orbit"
                         : (data.type == CameraControllerCodecType::Follow ? "follow" : "fly");
    result["damping"] = data.damping;
    if (data.type == CameraControllerCodecType::Orbit) {
        if (!data.target.empty()) result["target"] = data.target;
        result["distance"] = data.distance;
        result["yaw"] = data.yaw;
        result["pitch"] = data.pitch;
        result["sensitivity"] = data.sensitivity;
    } else if (data.type == CameraControllerCodecType::Follow) {
        result["target"] = data.target;
        result["offset"] = encodeVec(data.offset);
    } else {
        result["speed"] = data.speed;
        result["sensitivity"] = data.sensitivity;
    }
    return result;
}

OrderedJson encodeCamera(const ComponentCodecValue &value) {
    const auto &data = codecValue<CameraCodecData>(value, "camera");
    OrderedJson result{{"name", "camera"}};
    if (data.projection_specified) {
        result["type"] = cameraTypeName(data.projection_kind);
        if (data.projection_kind == CameraProjectionKind::Perspective) {
            if (data.yfov) result["yfov"] = *data.yfov;
            if (data.znear) result["znear"] = *data.znear;
            if (data.zfar) result["zfar"] = *data.zfar;
            if (data.aspect) result["aspect"] = *data.aspect;
        } else {
            if (data.xmag) result["xmag"] = *data.xmag;
            if (data.ymag) result["ymag"] = *data.ymag;
            if (data.znear) result["znear"] = *data.znear;
            if (data.zfar) result["zfar"] = *data.zfar;
        }
    }
    if (data.sprite_specified) {
        result["sprite"] = OrderedJson{
            {"pixel_perfect", data.pixel_perfect == CameraPixelPerfectMode::off ? "off" : "strict"},
            {"sort", data.sprite_sort == CameraSpriteSortPolicy::z
                         ? "z"
                         : (data.sprite_sort == CameraSpriteSortPolicy::y_down ? "y_down" : "declaration")}};
    }
    if (data.controller) result["controller"] = encodeController(*data.controller);
    return result;
}

void applyCamera(const ComponentCodecValue &value, void *target) {
    if (target == nullptr) throw std::invalid_argument("camera runtime target is null");
    *static_cast<CameraCodecData *>(target) = codecValue<CameraCodecData>(value, "camera");
}

ComponentCodecValue projectCamera(const void *source) {
    if (source == nullptr) throw std::invalid_argument("camera runtime source is null");
    return *static_cast<const CameraCodecData *>(source);
}

ComponentCodecValue decodeLight(const Json &json) {
    requireObjectAndName(json, "light");
    requireClosed(json, "light",
                  std::array<std::string_view, 9>{"name", "type", "position", "direction", "intensity",
                                                  "range", "color", "innerConeAngle", "outerConeAngle"});
    const auto type = readString(requireField(json, "light", "type"), "light.type");
    LightCodecData result;
    if (type == "directional") result.type = LightCodecType::Directional;
    else if (type == "point") result.type = LightCodecType::Point;
    else if (type == "spot") result.type = LightCodecType::Spot;
    else codecError(StructFieldErrorCode::InvalidEnum, "light.type", "is not directional, point, or spot");

    if (result.type != LightCodecType::Directional) {
        result.position = readVec3(requireField(json, "light", "position"), "light.position");
    } else if (json.contains("position")) {
        codecError(StructFieldErrorCode::UnknownField, "light.position", "is not valid for directional");
    }
    if (result.type != LightCodecType::Point) {
        result.direction = readVec3(requireField(json, "light", "direction"), "light.direction");
    } else if (json.contains("direction")) {
        codecError(StructFieldErrorCode::UnknownField, "light.direction", "is not valid for point");
    }
    result.color = readVec3(requireField(json, "light", "color"), "light.color", 0.0f, 1.0f);
    if (json.contains("intensity")) {
        result.intensity = readFloat(json.at("intensity"), "light.intensity", 0.0f);
    }
    if (json.contains("range")) {
        result.range = readFloat(json.at("range"), "light.range", 0.0f,
                                 std::numeric_limits<float>::max(), false);
    }
    if (result.type == LightCodecType::Spot) {
        if (json.contains("innerConeAngle")) {
            result.inner_cone_angle = readFloat(json.at("innerConeAngle"), "light.innerConeAngle", 0.0f, 180.0f);
        }
        if (json.contains("outerConeAngle")) {
            result.outer_cone_angle = readFloat(json.at("outerConeAngle"), "light.outerConeAngle", 0.0f, 180.0f);
        }
        if (result.inner_cone_angle > result.outer_cone_angle) {
            codecError(StructFieldErrorCode::OutOfRange, "light.innerConeAngle",
                       "must not exceed outerConeAngle");
        }
    } else if (json.contains("innerConeAngle") || json.contains("outerConeAngle")) {
        codecError(StructFieldErrorCode::UnknownField,
                   json.contains("innerConeAngle") ? "light.innerConeAngle" : "light.outerConeAngle",
                   "is valid only for spot");
    }
    return result;
}

std::string_view lightTypeName(LightCodecType type) {
    switch (type) {
    case LightCodecType::Directional: return "directional";
    case LightCodecType::Point: return "point";
    case LightCodecType::Spot: return "spot";
    }
    return "directional";
}

OrderedJson encodeLight(const ComponentCodecValue &value) {
    const auto &data = codecValue<LightCodecData>(value, "light");
    OrderedJson result{{"name", "light"}, {"type", lightTypeName(data.type)}};
    if (data.type != LightCodecType::Directional) result["position"] = encodeVec(data.position);
    if (data.type != LightCodecType::Point) result["direction"] = encodeVec(data.direction);
    result["intensity"] = data.intensity;
    if (data.range > 0.0f) result["range"] = data.range;
    result["color"] = encodeVec(data.color);
    if (data.type == LightCodecType::Spot) {
        result["innerConeAngle"] = data.inner_cone_angle;
        result["outerConeAngle"] = data.outer_cone_angle;
    }
    return result;
}

void applyLight(const ComponentCodecValue &value, void *target) {
    if (target == nullptr) throw std::invalid_argument("light runtime target is null");
    *static_cast<LightCodecData *>(target) = codecValue<LightCodecData>(value, "light");
}

ComponentCodecValue projectLight(const void *source) {
    if (source == nullptr) throw std::invalid_argument("light runtime source is null");
    return *static_cast<const LightCodecData *>(source);
}

ComponentCodecValue decodeCollider(const Json &json) {
    requireObjectAndName(json, "collider");
    requireClosed(json, "collider",
                  std::array<std::string_view, 13>{"name", "shape", "pos", "rotation", "radius",
                                                   "half_extents", "half_height", "layer", "mask", "trigger",
                                                   "one_way", "size", "height"});
    auto normalized = json;
    if (!normalized.contains("shape")) normalized["shape"] = "sphere";
    if (normalized.at("shape").is_string()) {
        const auto &shape = normalized.at("shape").get_ref<const std::string &>();
        if ((shape == "sphere" || shape == "capsule") &&
            !normalized.contains("radius")) {
            normalized["radius"] =
                componentCodecCanonicalDefaults().collider_radius;
        }
        if (shape == "box" && !normalized.contains("half_extents")) {
            normalized["half_extents"] = {0.5F, 0.5F, 0.5F};
        }
        if (shape == "capsule" && !normalized.contains("half_height")) {
            normalized["half_height"] = 0.5F;
        }
    }
    ColliderComponent result;
    JsonArchiveLoader archive{static_cast<const void *>(&normalized)};
    result.ref(archive);
    return result;
}

OrderedJson encodeCollider(const ComponentCodecValue &value) {
    const auto &data = codecValue<ColliderComponent>(value, "collider");
    data.validate();
    OrderedJson result{{"name", "collider"}, {"shape", data.shape}, {"pos", encodeVec(data.pos)},
                       {"rotation", encodeVec(data.rotation)}};
    if (data.shape == "sphere") result["radius"] = data.radius;
    else if (data.shape == "box") result["half_extents"] = encodeVec(data.half_extents);
    else if (data.shape == "capsule") {
        result["radius"] = data.radius;
        result["half_height"] = data.half_height;
    }
    result["layer"] = data.layer;
    result["mask"] = data.mask;
    result["trigger"] = data.trigger;
    result["one_way"] = data.one_way;
    return result;
}

void applyCollider(const ComponentCodecValue &value, void *target) {
    if (target == nullptr) throw std::invalid_argument("collider runtime target is null");
    *static_cast<ColliderComponent *>(target) = codecValue<ColliderComponent>(value, "collider");
}

ComponentCodecValue projectCollider(const void *source) {
    if (source == nullptr) throw std::invalid_argument("collider runtime source is null");
    return *static_cast<const ColliderComponent *>(source);
}

ComponentCodecValue decodeAnimation(const Json &json) {
    requireObjectAndName(json, "animation");
    if (json.contains("graph")) {
        legacyReject("animation.graph_reserved",
                     "animation: reserved v1 key 'graph' is not supported");
    }
    requireClosed(json, "animation",
                  std::array<std::string_view, 5>{"name", "clip", "speed", "loop", "start_time"});
    AnimationComponent result;
    result.clip = readString(requireField(json, "animation", "clip"), "animation.clip", true);
    result.speed = json.contains("speed") ? readDouble(json.at("speed"), "animation.speed", 0.0, false) : 1.0;
    result.start_time = json.contains("start_time")
                            ? readDouble(json.at("start_time"), "animation.start_time", 0.0)
                            : 0.0;
    const auto loop = json.contains("loop") ? readBool(json.at("loop"), "animation.loop") : true;
    result.loop = loop ? 1U : 0U;
    return result;
}

OrderedJson encodeAnimation(const ComponentCodecValue &value) {
    const auto &data = codecValue<AnimationComponent>(value, "animation");
    if (data.loop > 1U) codecError(StructFieldErrorCode::OutOfRange, "animation.loop", "must be boolean runtime state");
    return OrderedJson{{"name", "animation"}, {"clip", data.clip}, {"speed", data.speed},
                       {"loop", data.loop != 0}, {"start_time", data.start_time}};
}

void applyAnimation(const ComponentCodecValue &value, void *target) {
    if (target == nullptr) throw std::invalid_argument("animation runtime target is null");
    *static_cast<AnimationComponent *>(target) = codecValue<AnimationComponent>(value, "animation");
}

ComponentCodecValue projectAnimation(const void *source) {
    if (source == nullptr) throw std::invalid_argument("animation runtime source is null");
    return *static_cast<const AnimationComponent *>(source);
}

ComponentCodecValue decodeSprite(const Json &json) {
    // SpriteViewComponent owns the normative closed-schema parser. Calling it
    // here keeps the runtime loader and codec acceptance set identical.
    SpriteViewComponent result;
    JsonArchiveLoader archive{static_cast<const void *>(&json)};
    result.ref(archive);
    return result;
}

[[noreturn]] void rayInstanceCodecError(std::string_view legacy_branch_id,
                                        std::string_view code,
                                        std::string message) {
    legacyReject(legacy_branch_id,
                 "pelican." + std::string{code} +
                     "@1: " + std::move(message));
}

std::uint32_t decodeRayInstanceFlags(const Json &encoded) {
    if (!encoded.is_array()) {
        rayInstanceCodecError("pelican_ray_instance.flags_not_array",
                              "ray_instance_field_type",
                              "instance_flags must be a string array");
    }
    std::uint32_t result = 0;
    std::set<std::string> seen;
    for (const auto &item : encoded) {
        if (!item.is_string()) {
            rayInstanceCodecError("pelican_ray_instance.flag_not_string",
                                  "ray_instance_field_type",
                                  "instance_flags entries must be strings");
        }
        const auto &name = item.get_ref<const std::string &>();
        if (!seen.emplace(name).second) {
            rayInstanceCodecError("pelican_ray_instance.duplicate_flag",
                                  "ray_instance_field_type",
                                  "instance_flags contains duplicate '" +
                                      name + "'");
        }
        if (name == "triangle_facing_cull_disable") {
            result |= rayInstanceTriangleFacingCullDisable;
        } else if (name == "triangle_flip_facing") {
            result |= rayInstanceTriangleFlipFacing;
        } else if (name == "force_opaque") {
            result |= rayInstanceForceOpaque;
        } else if (name == "force_no_opaque") {
            result |= rayInstanceForceNoOpaque;
        } else {
            rayInstanceCodecError("pelican_ray_instance.unknown_flag",
                                  "ray_as_unknown_flag",
                                  "unknown instance flag '" + name + "'");
        }
    }
    if ((result & rayInstanceForceOpaque) != 0 &&
        (result & rayInstanceForceNoOpaque) != 0) {
        rayInstanceCodecError("pelican_ray_instance.conflicting_flags",
                              "ray_as_conflicting_instance_flags",
                              "force_opaque and force_no_opaque are mutually exclusive");
    }
    return result;
}

std::uint32_t decodeRayInstanceUnsigned(const Json &encoded,
                                        std::uint32_t maximum,
                                        std::string_view field) {
    std::uint64_t result = 0;
    if (encoded.is_number_unsigned()) {
        result = encoded.get<std::uint64_t>();
    } else if (encoded.is_number_integer() &&
               encoded.get<std::int64_t>() >= 0) {
        result = static_cast<std::uint64_t>(encoded.get<std::int64_t>());
    } else {
        rayInstanceCodecError("pelican_ray_instance.unsigned_type",
                              "ray_instance_field_type",
                              std::string{field} +
                                  " must be an unsigned integer");
    }
    if (result > maximum) {
        rayInstanceCodecError("pelican_ray_instance.unsigned_range",
                              "ray_instance_field_range",
                              std::string{field} + " is out of range");
    }
    return static_cast<std::uint32_t>(result);
}

ComponentCodecValue decodeRayInstance(const Json &json) {
    if (!json.is_object()) {
        rayInstanceCodecError("pelican_ray_instance.payload_not_object",
                              "ray_instance_field_type",
                              "pelican_ray_instance must be an object");
    }
    const auto name = json.find("name");
    if (name == json.end() || !name->is_string() ||
        name->get_ref<const std::string &>() != "pelican_ray_instance") {
        rayInstanceCodecError("pelican_ray_instance.name_mismatch",
                              "ray_instance_field_type",
                              "name must be 'pelican_ray_instance'");
    }
    for (const auto &[key, value] : json.items()) {
        (void)value;
        if (key == "overrides") {
            legacyReject(
                "pelican_ray_instance.overrides_not_enabled",
                "ray_instance_overrides_not_enabled@1: per-occurrence "
                "overrides require WP382d enablement");
        }
        if (key != "name" && key != "mask" &&
            key != "instance_flags" && key != "custom_index") {
            rayInstanceCodecError("pelican_ray_instance.unknown_field",
                                  "ray_instance_unknown_field",
                                  "unknown field '" + key + "'");
        }
    }
    RayInstanceComponent result;
    if (const auto mask = json.find("mask"); mask != json.end()) {
        result.mask = static_cast<std::uint8_t>(decodeRayInstanceUnsigned(
            *mask, 0xffu, "mask"));
    }
    if (const auto flags = json.find("instance_flags");
        flags != json.end()) {
        result.instance_flags = decodeRayInstanceFlags(*flags);
    }
    if (const auto custom = json.find("custom_index");
        custom != json.end()) {
        result.custom_index = decodeRayInstanceUnsigned(
            *custom, rayCustomIndexMaximum, "custom_index");
    }
    if (!result.mask && !result.instance_flags && !result.custom_index) {
        rayInstanceCodecError("pelican_ray_instance.empty",
                              "ray_instance_empty",
                              "pelican_ray_instance must declare at least one field");
    }
    return result;
}

OrderedJson encodeRayInstance(const ComponentCodecValue &value) {
    const auto &data =
        codecValue<RayInstanceComponent>(value, "pelican_ray_instance");
    OrderedJson result{{"name", "pelican_ray_instance"}};
    if (data.mask) result["mask"] = *data.mask;
    if (data.instance_flags) {
        OrderedJson flags = OrderedJson::array();
        if ((*data.instance_flags & rayInstanceTriangleFacingCullDisable) != 0)
            flags.push_back("triangle_facing_cull_disable");
        if ((*data.instance_flags & rayInstanceTriangleFlipFacing) != 0)
            flags.push_back("triangle_flip_facing");
        if ((*data.instance_flags & rayInstanceForceOpaque) != 0)
            flags.push_back("force_opaque");
        if ((*data.instance_flags & rayInstanceForceNoOpaque) != 0)
            flags.push_back("force_no_opaque");
        result["instance_flags"] = std::move(flags);
    }
    if (data.custom_index) result["custom_index"] = *data.custom_index;
    return result;
}

void applyRayInstance(const ComponentCodecValue &value, void *target) {
    if (target == nullptr)
        throw std::invalid_argument(
            "pelican_ray_instance runtime target is null");
    *static_cast<RayInstanceComponent *>(target) =
        codecValue<RayInstanceComponent>(value, "pelican_ray_instance");
}

ComponentCodecValue projectRayInstance(const void *source) {
    if (source == nullptr)
        throw std::invalid_argument(
            "pelican_ray_instance runtime source is null");
    return *static_cast<const RayInstanceComponent *>(source);
}

OrderedJson encodeSprite(const ComponentCodecValue &value) {
    const auto &data = codecValue<SpriteViewComponent>(value, "sprite_view");
    data.validate();
    OrderedJson result{{"name", "sprite_view"}, {"texture", data.texture}};
    if (data.has_explicit_size) result["size"] = encodeVec(data.size);
    result["pivot"] = encodeVec(data.pivot);
    result["color"] = encodeVec(data.color);
    result["flip"] = OrderedJson::array({data.flip_x != 0, data.flip_y != 0});
    result["layer"] = data.layer;
    result["billboard"] = data.billboard == SpriteBillboard::none
                                ? "none"
                                : (data.billboard == SpriteBillboard::y_axis ? "y_axis" : "full");
    return result;
}

void applySprite(const ComponentCodecValue &value, void *target) {
    if (target == nullptr) throw std::invalid_argument("sprite_view runtime target is null");
    *static_cast<SpriteViewComponent *>(target) = codecValue<SpriteViewComponent>(value, "sprite_view");
}

ComponentCodecValue projectSprite(const void *source) {
    if (source == nullptr) throw std::invalid_argument("sprite_view runtime source is null");
    return *static_cast<const SpriteViewComponent *>(source);
}

template <class Fields> std::span<const StructFieldSchema> fieldSpan(const Fields &fields) {
    return {fields.data(), fields.size()};
}

template <class Component>
ComponentEditorSwapToken prepareEcsEditorSwap(
    void *raw_ecs, std::uint32_t entity_index,
    std::uint32_t entity_generation, const ComponentCodecValue &value) {
    if (raw_ecs == nullptr) {
        throw std::invalid_argument("editor codec ECS target is null");
    }
    auto &ecs = *static_cast<ECSCoreTemplatePublic *>(raw_ecs);
    using Token = ECSCoreTemplatePublic::PreparedComponentSwap<Component>;
    return ComponentEditorSwapToken::own(std::make_unique<Token>(
        ecs.prepareComponentSwap<Component>(
            EntityId{entity_index, entity_generation},
            codecValue<Component>(value, "editor projection"))));
}

template <class Component>
void swapEcsEditorToken(ComponentEditorSwapToken &erased,
                        EditorCodecSwapPhase phase) noexcept {
    using Token = ECSCoreTemplatePublic::PreparedComponentSwap<Component>;
    auto &token = erased.get<Token>();
    switch (phase) {
    case EditorCodecSwapPhase::Publish:
        token.publish();
        break;
    case EditorCodecSwapPhase::Rollback:
        token.rollback();
        break;
    case EditorCodecSwapPhase::Finish:
        token.finish();
        break;
    }
}

constexpr std::array legacy_branch_descriptors{
    LegacyBranchDescriptor{"animation.graph_reserved", "decodeAnimation"},
    LegacyBranchDescriptor{"animation.schema_validation", "decodeAnimation"},
    LegacyBranchDescriptor{"camera.schema_validation", "decodeCamera"},
    LegacyBranchDescriptor{"collider.boolean_type", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.box_size_alias", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.capsule_height_alias", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.capsule_radius_non_positive", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.half_extents_non_positive", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.half_height_negative", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.numeric_type", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.pose_non_finite", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.quaternion_type", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.radius_non_positive", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.schema_validation", "decodeCollider"},
    LegacyBranchDescriptor{"collider.shape_type", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.shape_unknown", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.uint32_range", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.uint32_type", "ColliderComponent"},
    LegacyBranchDescriptor{"collider.vec3_type", "ColliderComponent"},
    LegacyBranchDescriptor{"light.schema_validation", "decodeLight"},
    LegacyBranchDescriptor{"pelican_ray_instance.conflicting_flags", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.duplicate_flag", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.empty", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.flag_not_string", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.flags_not_array", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.name_mismatch", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.overrides_not_enabled", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.payload_not_object", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.unknown_field", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.unknown_flag", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.unsigned_range", "decodeRayInstance"},
    LegacyBranchDescriptor{"pelican_ray_instance.unsigned_type", "decodeRayInstance"},
    LegacyBranchDescriptor{"simplemodelview.schema_validation", "decodeSimpleModelView"},
    LegacyBranchDescriptor{"sprite_view.billboard_type", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.billboard_value", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.color_non_finite", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.flip_type", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.layer_range", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.layer_type", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.name_mismatch", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.payload_not_object", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.pivot_out_of_range", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.size_non_positive", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.texture_empty", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.texture_fragment_empty", "parseSpriteAssetReference"},
    LegacyBranchDescriptor{"sprite_view.texture_fragment_syntax", "parseSpriteAssetReference"},
    LegacyBranchDescriptor{"sprite_view.texture_multiple_fragments", "parseSpriteAssetReference"},
    LegacyBranchDescriptor{"sprite_view.texture_snapshot_reserved", "parseSpriteAssetReference"},
    LegacyBranchDescriptor{"sprite_view.texture_type", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.unknown_field", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.vec2_type", "SpriteViewComponent"},
    LegacyBranchDescriptor{"sprite_view.vec4_type", "SpriteViewComponent"},
    LegacyBranchDescriptor{"transform.schema_validation", "decodeTransform"},
};

consteval bool legacyBranchDescriptorsAreStrictlyOrdered() {
    for (std::size_t index = 1; index < legacy_branch_descriptors.size();
         ++index) {
        if (!(legacy_branch_descriptors[index - 1].legacy_branch_id <
              legacy_branch_descriptors[index].legacy_branch_id)) {
            return false;
        }
    }
    return true;
}

static_assert(legacyBranchDescriptorsAreStrictlyOrdered(),
              "component codec legacy branch IDs must be unique and sorted");

const std::array codecs{
    ComponentCodec{"transform", ComponentCodecRuntimeKind::Ecs,
                   EditorProjectionPolicy::TransformClosure,
                   decodeResult<decodeTransform>, encodeTransform,
                    []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(transform_fields)); return node; }, applyTransform,
                   projectTransform, nullptr, nullptr},
    ComponentCodec{"simplemodelview", ComponentCodecRuntimeKind::Ecs,
                   EditorProjectionPolicy::RendererModel,
                   decodeResult<decodeSimpleModelView>,
                    encodeSimpleModelView, []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(simple_model_view_fields)); return node; },
                   applySimpleModelView, projectSimpleModelView, nullptr, nullptr},
    ComponentCodec{"pelican_procedural_geometry",
                   ComponentCodecRuntimeKind::Ecs,
                   EditorProjectionPolicy::EcsCodec,
                   decodeResult<decodeProceduralGeometry>,
                   encodeProceduralGeometry,
                   []() -> const SchemaNode & {
                       static const auto node = SchemaNode::fromFlat(
                           fieldSpan(procedural_geometry_fields));
                       return node;
                   },
                   applyProceduralGeometry, projectProceduralGeometry,
                   prepareEcsEditorSwap<ProceduralGeometryComponent>,
                   swapEcsEditorToken<ProceduralGeometryComponent>},
    ComponentCodec{"camera", ComponentCodecRuntimeKind::Camera,
                   EditorProjectionPolicy::Camera, decodeResult<decodeCamera>,
                   encodeCamera,
                    []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(camera_fields)); return node; }, applyCamera,
                   projectCamera, nullptr, nullptr},
    ComponentCodec{"light", ComponentCodecRuntimeKind::Light,
                   EditorProjectionPolicy::Light, decodeResult<decodeLight>,
                   encodeLight,
                    []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(light_fields)); return node; }, applyLight,
                   projectLight, nullptr, nullptr},
    ComponentCodec{"collider", ComponentCodecRuntimeKind::Collider,
                   EditorProjectionPolicy::Collider,
                   decodeResult<decodeCollider>,
                    encodeCollider, []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(collider_fields)); return node; },
                   applyCollider, projectCollider, nullptr, nullptr},
    ComponentCodec{"animation", ComponentCodecRuntimeKind::Ecs,
                   EditorProjectionPolicy::EcsCodec,
                   decodeResult<decodeAnimation>,
                    encodeAnimation, []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(animation_fields)); return node; },
                   applyAnimation, projectAnimation,
                   prepareEcsEditorSwap<AnimationComponent>,
                   swapEcsEditorToken<AnimationComponent>},
    ComponentCodec{"sprite_view", ComponentCodecRuntimeKind::Ecs,
                   EditorProjectionPolicy::EcsCodec,
                   decodeResult<decodeSprite>, encodeSprite,
                    []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(sprite_fields)); return node; }, applySprite,
                   projectSprite, prepareEcsEditorSwap<SpriteViewComponent>,
                   swapEcsEditorToken<SpriteViewComponent>},
    ComponentCodec{"pelican_ray_instance", ComponentCodecRuntimeKind::Ecs,
                   EditorProjectionPolicy::EcsCodec,
                   decodeResult<decodeRayInstance>, encodeRayInstance,
                    []() -> const SchemaNode & { static const auto node = SchemaNode::fromFlat(fieldSpan(ray_instance_fields)); return node; },
                   applyRayInstance, projectRayInstance,
                   prepareEcsEditorSwap<RayInstanceComponent>,
                   swapEcsEditorToken<RayInstanceComponent>},
};

} // namespace

std::span<const LegacyBranchDescriptor> componentCodecLegacyBranches() {
    static const bool runtime_validated = [] {
        for (std::size_t left = 0; left < legacy_branch_descriptors.size();
             ++left) {
            for (std::size_t right = left + 1;
                 right < legacy_branch_descriptors.size(); ++right) {
                if (legacy_branch_descriptors[left].legacy_branch_id ==
                    legacy_branch_descriptors[right].legacy_branch_id) {
                    throw std::logic_error(
                        "duplicate component codec legacy branch ID");
                }
            }
        }
        return true;
    }();
    (void)runtime_validated;
    return legacy_branch_descriptors;
}

std::optional<UserDataViolation> completedCameraCrossFieldViolation(
    const CameraCodecData &camera,
    std::span<const std::string> path_prefix) {
    if (!camera.znear || !camera.zfar) return std::nullopt;
    return cameraCrossFieldViolation(*camera.znear, *camera.zfar,
                                     path_prefix);
}

ComponentCodecCanonicalDefaults componentCodecCanonicalDefaults() noexcept {
    return codec_canonical_defaults;
}

namespace testing {

ScopedComponentCodecCanonicalDefaults::ScopedComponentCodecCanonicalDefaults(
    ComponentCodecCanonicalDefaults replacement)
    : previous_{codec_canonical_defaults} {
    if (!std::isfinite(replacement.collider_radius) ||
        replacement.collider_radius < 0.0f) {
        throw std::invalid_argument(
            "collider codec radius default must be finite and non-negative");
    }
    codec_canonical_defaults = replacement;
}

ScopedComponentCodecCanonicalDefaults::~ScopedComponentCodecCanonicalDefaults()
    noexcept {
    codec_canonical_defaults = previous_;
}

} // namespace testing

ResolutionResult<ComponentCodecValue> ComponentCodec::decodeAuthored(
    const nlohmann::json &authored) const {
    if (decode_authored == nullptr) throw std::logic_error("component codec decode callback is missing");
    try {
        return decode_authored(authored);
    } catch (const StructFieldValidationError &error) {
        legacyReject(std::string{name} + ".schema_validation", error.what());
    }
}

nlohmann::ordered_json ComponentCodec::encodeCanonical(const ComponentCodecValue &value) const {
    if (encode_canonical == nullptr) throw std::logic_error("component codec encode callback is missing");
    return encode_canonical(value);
}

const SchemaNode &ComponentCodec::schemaNode() const {
    if (schema == nullptr) throw std::logic_error("component codec schema callback is missing");
    return schema();
}

std::vector<StructFieldSchema> ComponentCodec::fieldSchema() const {
    std::vector<StructFieldSchema> result;
    const auto &root = schemaNode();
    result.reserve(root.childCount(0));
    for (std::size_t ordinal = 0; ordinal < root.childCount(0); ++ordinal) {
        const auto &node = root.at(root.childIndex(0, ordinal));
        StructFieldSchema field{node.name, node.leaf_type,
                                materializeStructFieldRange(node.range),
                                node.unit};
        field.enum_value_count =
            std::min<std::size_t>(node.vocabulary_count,
                                  maxStructFieldEnumValues);
        for (std::size_t index = 0; index < field.enum_value_count; ++index)
            field.enum_values[index] = node.vocabulary[index].name;
        result.push_back(std::move(field));
    }
    return result;
}

void ComponentCodec::applyRuntime(const ComponentCodecValue &value, void *target) const {
    if (runtime_apply == nullptr) throw std::logic_error("component codec runtime apply callback is missing");
    runtime_apply(value, target);
}

ComponentCodecValue ComponentCodec::projectRuntime(const void *source) const {
    if (runtime_project == nullptr) throw std::logic_error("component codec runtime project callback is missing");
    return runtime_project(source);
}

ComponentEditorSwapToken ComponentCodec::prepareEditorSwap(
    void *ecs, std::uint32_t entity_index,
    std::uint32_t entity_generation,
    const ComponentCodecValue &value) const {
    if (editor_policy != EditorProjectionPolicy::EcsCodec ||
        editor_prepare == nullptr || editor_swap == nullptr) {
        throw std::logic_error(
            "component codec editor projection callbacks are missing");
    }
    return editor_prepare(ecs, entity_index, entity_generation, value);
}

void ComponentCodec::swapEditorToken(
    ComponentEditorSwapToken &token, EditorCodecSwapPhase phase) const noexcept {
    if (editor_swap != nullptr) editor_swap(token, phase);
}

namespace {
bool rayInstanceCodecEnabled() noexcept {
    const auto *state =
        FastModuleContainer::tryGet<RayLayoutRuntimeState>();
    return state != nullptr && state->builtinRayInstanceEnabled();
}
} // namespace

std::span<const ComponentCodec> componentCodecs() {
    (void)componentCodecLegacyBranches();
    if (codec_catalog_override != nullptr) return *codec_catalog_override;
    return rayInstanceCodecEnabled()
               ? std::span<const ComponentCodec>{codecs}
               : std::span<const ComponentCodec>{codecs}.first(
                     codecs.size() - 1u);
}

namespace testing {

const ComponentCodec *findComponentCodecIncludingDisabled(
    std::string_view name) noexcept {
    const auto found = std::find_if(
        codecs.begin(), codecs.end(), [name](const ComponentCodec &codec) {
            return codec.name == name;
        });
    return found == codecs.end() ? nullptr : &*found;
}

ScopedComponentCodecCatalog::ScopedComponentCodecCatalog(
    std::vector<ComponentCodec> replacement)
    : replacement_{std::move(replacement)},
      previous_{codec_catalog_override} {
    if (replacement_.empty()) {
        throw std::invalid_argument(
            "component codec test catalog must not be empty");
    }
    codec_catalog_override = &replacement_;
}

ScopedComponentCodecCatalog::~ScopedComponentCodecCatalog() noexcept {
    codec_catalog_override = previous_;
}

} // namespace testing

const ComponentCodec *findComponentCodec(std::string_view name) noexcept {
    const auto available = componentCodecs();
    const auto found = std::find_if(available.begin(), available.end(),
                                    [&](const ComponentCodec &codec) { return codec.name == name; });
    return found == available.end() ? nullptr : &*found;
}

const ComponentCodec &requireComponentCodec(std::string_view name) {
    if (const auto *codec = findComponentCodec(name)) return *codec;
    throw std::out_of_range("component codec is not registered: " + std::string{name});
}

ComponentCodecQueryMetadata componentCodecQueryMetadata(std::string_view name) noexcept {
    if (const auto *codec = findComponentCodec(name)) {
        return {ComponentCodecState::Registered, true, codec->name};
    }
    return {ComponentCodecState::Missing, false, {}};
}

nlohmann::ordered_json projectTransformRuntimeJson(const TransformCodecTarget &target) {
    const auto &codec = requireComponentCodec("transform");
    const auto local = codec.encodeCanonical(codec.projectRuntime(&target));
    if (target.world == nullptr) throw std::invalid_argument("transform world source is null");
    const TransformCodecData world{{target.world->pos.x, target.world->pos.y, target.world->pos.z},
                                   {target.world->rotation.x, target.world->rotation.y,
                                    target.world->rotation.z, target.world->rotation.w},
                                   {target.world->scale.x, target.world->scale.y, target.world->scale.z}, true};
    return OrderedJson{{"local_trs", local},
                       {"world_trs", codec.encodeCanonical(ComponentCodecValue{world})}};
}

} // namespace Pelican
