#define GLM_ENABLE_EXPERIMENTAL
#include "editorpreviewprojection.hpp"

#include "componentcodec.hpp"
#include "../phys/physworld.hpp"
#include "../userpublic/components/collider.hpp"

#include <algorithm>
#include <any>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

[[noreturn]] void schemaError(std::string field, std::string message) {
    throw EditorPreviewProjectionError{
        EditorPreviewProjectionErrorCode::schema_violation,
        std::move(field), "prepared_projection", std::move(message)};
}

[[noreturn]] void unavailable(std::string field, std::string adapter,
                              std::string message) {
    throw EditorPreviewProjectionError{
        EditorPreviewProjectionErrorCode::method_unavailable,
        std::move(field), std::move(adapter), std::move(message)};
}

void requireObject(const Json &value, std::string_view field) {
    if (!value.is_object()) {
        schemaError(std::string{field}, std::string{field} + " must be an object");
    }
}

void requireOnly(const Json &value,
                 std::initializer_list<std::string_view> fields,
                 std::string_view context) {
    requireObject(value, context);
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (std::find(fields.begin(), fields.end(), it.key()) == fields.end()) {
            schemaError(std::string{context} + "/" + it.key(),
                        std::string{context} + " contains unknown field '" +
                            it.key() + "'");
        }
    }
}

std::uint64_t exactUnsigned(const Json &value, std::string_view field) {
    std::uint64_t result = 0;
    if (value.is_number_unsigned()) {
        result = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            schemaError(std::string{field}, std::string{field} +
                                                " must be a non-negative integer");
        }
        result = static_cast<std::uint64_t>(signed_value);
    } else {
        schemaError(std::string{field}, std::string{field} +
                                            " must be a non-negative integer");
    }
    if (result > UINT64_C(9007199254740991)) {
        schemaError(std::string{field}, std::string{field} + " exceeds 2^53-1");
    }
    return result;
}

std::string requiredString(const Json &value, std::string_view field,
                           std::string_view context) {
    const auto found = value.find(std::string{field});
    if (found == value.end() || !found->is_string() || found->empty()) {
        schemaError(std::string{context} + "/" + std::string{field},
                    std::string{context} + " requires non-empty string field '" +
                        std::string{field} + "'");
    }
    return found->get<std::string>();
}

glm::vec3 toGlm(vec3 value) { return {value.x, value.y, value.z}; }
glm::quat toGlm(quat value) { return {value.w, value.x, value.y, value.z}; }
vec3 fromGlm(glm::vec3 value) { return {value.x, value.y, value.z}; }
quat fromGlm(glm::quat value) { return {value.x, value.y, value.z, value.w}; }

OrderedJson vecJson(glm::vec3 value) {
    return OrderedJson::array({value.x, value.y, value.z});
}

OrderedJson quatJson(glm::quat value) {
    return OrderedJson::array({value.x, value.y, value.z, value.w});
}

OrderedJson matrixJson(const glm::mat4 &matrix) {
    auto result = OrderedJson::array();
    for (std::size_t row = 0; row < 4; ++row) {
        auto values = OrderedJson::array();
        for (std::size_t column = 0; column < 4; ++column) {
            values.push_back(matrix[column][row]);
        }
        result.push_back(std::move(values));
    }
    return result;
}

struct EvaluatedTrs {
    glm::vec3 pos{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

OrderedJson trsJson(const EvaluatedTrs &value) {
    return OrderedJson{{"pos", vecJson(value.pos)},
                       {"rotation", quatJson(value.rotation)},
                       {"scale", vecJson(value.scale)}};
}

EvaluatedTrs decodeLocalTrs(const Json &object) {
    if (!object.contains("components") || !object.at("components").is_array()) {
        return {};
    }
    for (const auto &component : object.at("components")) {
        if (!component.is_object() ||
            component.value("name", std::string{}) != "transform") {
            continue;
        }
        const auto decoded = requireComponentCodec("transform").decodeAuthored(component);
        const auto &value = std::any_cast<const TransformCodecData &>(decoded);
        auto rotation = toGlm(value.rotation);
        const auto length = glm::length(rotation);
        if (!std::isfinite(length) || length <= 1.0e-8f) {
            schemaError("overrides/transform/rotation",
                        "transform rotation must be finite and non-zero");
        }
        return {.pos = toGlm(value.pos),
                .rotation = glm::normalize(rotation),
                .scale = toGlm(value.scale)};
    }
    return {};
}

EvaluatedTrs compose(const EvaluatedTrs &parent, const EvaluatedTrs &local) {
    return {
        .pos = parent.pos + parent.rotation * (parent.scale * local.pos),
        .rotation = glm::normalize(parent.rotation * local.rotation),
        .scale = parent.scale * local.scale,
    };
}

struct ObjectLocation {
    std::size_t scene_index = 0;
    std::size_t object_index = 0;
    std::string scene_id;
};

std::unordered_map<std::uint64_t, ObjectLocation>
objectLocations(const AuthoringSceneDocument &document) {
    std::unordered_map<std::uint64_t, ObjectLocation> result;
    const auto scenes = document.query();
    for (std::size_t scene_index = 0; scene_index < scenes.size(); ++scene_index) {
        const auto &scene = scenes[scene_index];
        for (std::size_t object_index = 0; object_index < scene.objects.size();
             ++object_index) {
            const auto &object = scene.objects[object_index];
            result.emplace(object.authoring_object_id.value,
                           ObjectLocation{scene_index, object_index,
                                          scene.scene_id});
        }
    }
    return result;
}

Json *findComponent(Json &object, std::string_view name) {
    if (!object.contains("components") || !object.at("components").is_array()) {
        return nullptr;
    }
    for (auto &component : object.at("components")) {
        if (component.is_object() &&
            component.value("name", std::string{}) == name) {
            return &component;
        }
    }
    return nullptr;
}

const Json *findComponent(const Json &object, std::string_view name) {
    if (!object.contains("components") || !object.at("components").is_array()) {
        return nullptr;
    }
    for (const auto &component : object.at("components")) {
        if (component.is_object() &&
            component.value("name", std::string{}) == name) {
            return &component;
        }
    }
    return nullptr;
}

void applyOverrides(Json &raw, const AuthoringSceneDocument &base,
                    const Json &overrides,
                    const EditorPreviewProjectionFaultHook &fault_hook) {
    if (!overrides.is_array()) {
        schemaError("overrides", "eval_preview overrides must be an array");
    }
    const auto locations = objectLocations(base);
    for (std::size_t index = 0; index < overrides.size(); ++index) {
        if (fault_hook) fault_hook("prepare", index);
        const auto &override_value = overrides.at(index);
        requireOnly(override_value,
                    {"op", "object_id", "component_slot", "field_path", "value"},
                    "overrides/" + std::to_string(index));
        const auto context = "overrides/" + std::to_string(index);
        if (requiredString(override_value, "op", context) !=
            "set_component_value") {
            unavailable(context + "/op", "prepared_projection",
                        "eval_preview v1 supports only set_component_value overrides");
        }
        if (!override_value.contains("object_id")) {
            schemaError(context + "/object_id", "override requires object_id");
        }
        const auto object_id = exactUnsigned(override_value.at("object_id"),
                                             context + "/object_id");
        const auto located = locations.find(object_id);
        if (located == locations.end()) {
            schemaError(context + "/object_id",
                        "override object_id is not present in the immutable base document");
        }
        const auto component_name =
            requiredString(override_value, "component_slot", context);
        const auto *codec = findComponentCodec(component_name);
        if (codec == nullptr) {
            unavailable(context + "/component_slot", component_name,
                        "component has no prepared projection codec");
        }
        auto &object = raw.at("scenes").at(located->second.scene_id)
                           .at("objects").at(located->second.object_index);
        auto *component = findComponent(object, component_name);
        if (component == nullptr) {
            schemaError(context + "/component_slot",
                        "override component_slot is absent on the object");
        }
        const auto field_path = requiredString(override_value, "field_path", context);
        if (field_path.empty() || field_path.front() != '/' ||
            field_path == "/name" || field_path.starts_with("/name/")) {
            schemaError(context + "/field_path",
                        "field_path must be a JSON pointer and cannot change name");
        }
        if (!override_value.contains("value")) {
            schemaError(context + "/value", "override requires value");
        }
        try {
            (*component)[Json::json_pointer{field_path}] = override_value.at("value");
            const auto decoded = codec->decodeAuthored(*component);
            *component = Json::parse(codec->encodeCanonical(decoded).dump());
        } catch (const EditorPreviewProjectionError &) {
            throw;
        } catch (const std::exception &error) {
            schemaError(context + "/field_path", error.what());
        }
    }
}

OrderedJson evaluatedScene(const AuthoringSceneDocument &document) {
    auto result = OrderedJson{{"scenes", OrderedJson::array()},
                              {"colliders", OrderedJson::array()}};
    for (const auto &scene : document.query()) {
        struct Node {
            const AuthoringObjectView *view = nullptr;
            EvaluatedTrs local;
            EvaluatedTrs world;
            enum class Visit : std::uint8_t { fresh, active, done } visit = Visit::fresh;
        };
        std::vector<Node> nodes;
        nodes.reserve(scene.objects.size());
        std::unordered_map<std::string, std::size_t> by_name;
        for (const auto &object : scene.objects) {
            nodes.push_back(Node{.view = &object,
                                 .local = decodeLocalTrs(object.authoredJson())});
            if (object.name) by_name.emplace(*object.name, nodes.size() - 1);
        }
        const auto resolve = [&](auto &&self, std::size_t index) -> void {
            auto &node = nodes.at(index);
            if (node.visit == Node::Visit::done) return;
            if (node.visit == Node::Visit::active) {
                schemaError("prepared_projection/parent",
                            "prepared transform hierarchy contains a cycle");
            }
            node.visit = Node::Visit::active;
            if (node.view->parent) {
                const auto parent = by_name.find(*node.view->parent);
                if (parent == by_name.end()) {
                    schemaError("prepared_projection/parent",
                                "prepared transform parent is not present in the scene");
                }
                self(self, parent->second);
                node.world = compose(nodes.at(parent->second).world, node.local);
            } else {
                node.world = node.local;
            }
            node.visit = Node::Visit::done;
        };

        auto evaluated_objects = OrderedJson::array();
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            resolve(resolve, index);
            const auto &node = nodes[index];
            auto components = OrderedJson::array();
            for (const auto &component : node.view->components) {
                const auto name = component.authoredJson().value("name", std::string{});
                if (const auto *codec = findComponentCodec(name)) {
                    components.push_back(codec->encodeCanonical(
                        codec->decodeAuthored(component.authoredJson())));
                } else {
                    components.push_back(component.authoredJson());
                }
            }
            auto evaluated = OrderedJson{
                {"object_id", node.view->authoring_object_id.value},
                {"name", node.view->name ? OrderedJson(*node.view->name)
                                         : OrderedJson(nullptr)},
                {"parent", node.view->parent ? OrderedJson(*node.view->parent)
                                             : OrderedJson(nullptr)},
                {"local_trs", trsJson(node.local)},
                {"world_trs", trsJson(node.world)},
                {"components", std::move(components)},
            };
            evaluated_objects.push_back(evaluated);

            if (const auto *authored_collider =
                    findComponent(node.view->authoredJson(), "collider")) {
                ColliderComponent collider;
                const auto &codec = requireComponentCodec("collider");
                codec.applyRuntime(codec.decodeAuthored(*authored_collider), &collider);
                const auto stable_collider_id =
                    (node.view->authoring_object_id.value << 8U) | UINT64_C(1);
                const std::string name = node.view->name.value_or(
                    "object-" + std::to_string(node.view->authoring_object_id.value));
                const auto built = buildPhysColliders(std::array{PhysWorldColliderInput{
                    .name = name,
                    .collider = collider,
                    .transform = PhysWorldTransform{
                        .pos = fromGlm(node.world.pos),
                        .rotation = fromGlm(node.world.rotation),
                        .scale = fromGlm(node.world.scale),
                    },
                    .identity = phys::ColliderIdentity{
                        .collider_id = phys::ColliderId{stable_collider_id},
                        .name = name,
                    },
                }});
                const auto &shape = built.front().shape;
                OrderedJson shape_json;
                if (const auto *sphere = std::get_if<phys::Sphere>(&shape)) {
                    shape_json = {{"type", "sphere"},
                                  {"center", OrderedJson::array(
                                       {sphere->center.x, sphere->center.y,
                                        sphere->center.z})},
                                  {"radius", sphere->radius}};
                } else if (const auto *box = std::get_if<phys::Box>(&shape)) {
                    shape_json = {{"type", "box"},
                                  {"center", OrderedJson::array(
                                       {box->center.x, box->center.y, box->center.z})},
                                  {"rotation", OrderedJson::array(
                                       {box->rotation.x, box->rotation.y,
                                        box->rotation.z, box->rotation.w})},
                                  {"half_extents", OrderedJson::array(
                                       {box->half_extents.x, box->half_extents.y,
                                        box->half_extents.z})}};
                } else {
                    const auto &capsule = std::get<phys::Capsule>(shape);
                    shape_json = {{"type", "capsule"},
                                  {"center", OrderedJson::array(
                                       {capsule.center.x, capsule.center.y,
                                        capsule.center.z})},
                                  {"rotation", OrderedJson::array(
                                       {capsule.rotation.x, capsule.rotation.y,
                                        capsule.rotation.z, capsule.rotation.w})},
                                  {"half_height", capsule.half_height},
                                  {"radius", capsule.radius}};
                }
                result["colliders"].push_back({
                    {"object_id", node.view->authoring_object_id.value},
                    {"id", stable_collider_id},
                    {"name", name},
                    {"shape", std::move(shape_json)},
                    {"layer", collider.layer},
                    {"mask", collider.mask},
                    {"trigger", collider.trigger},
                    {"one_way", collider.one_way},
                });
            }
        }
        result["scenes"].push_back({{"scene_id", scene.scene_id},
                                    {"objects", std::move(evaluated_objects)}});
    }
    return result;
}

const OrderedJson &findEvaluatedObject(const OrderedJson &scene,
                                       std::uint64_t object_id,
                                       std::string_view field) {
    for (const auto &entry : scene.at("scenes")) {
        for (const auto &object : entry.at("objects")) {
            if (object.at("object_id").get<std::uint64_t>() == object_id) {
                return object;
            }
        }
    }
    schemaError(std::string{field}, "query object_id is not present");
}

const OrderedJson *findEvaluatedComponent(const OrderedJson &object,
                                          std::string_view name) {
    for (const auto &component : object.at("components")) {
        if (component.value("name", std::string{}) == name) return &component;
    }
    return nullptr;
}

vec3 queryVec3(const Json &value, std::string_view field) {
    if (!value.is_array() || value.size() != 3) {
        schemaError(std::string{field}, std::string{field} + " must be a vec3 array");
    }
    vec3 result;
    try {
        result = {value.at(0).get<float>(), value.at(1).get<float>(),
                  value.at(2).get<float>()};
    } catch (const std::exception &) {
        schemaError(std::string{field}, std::string{field} +
                                            " must contain finite numbers");
    }
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z)) {
        schemaError(std::string{field}, std::string{field} +
                                            " must contain finite numbers");
    }
    return result;
}

quat queryQuat(const Json &value, std::string_view field) {
    if (!value.is_array() || value.size() != 4) {
        schemaError(std::string{field}, std::string{field} + " must be a quat array");
    }
    quat result;
    try {
        result = {value.at(0).get<float>(), value.at(1).get<float>(),
                  value.at(2).get<float>(), value.at(3).get<float>()};
    } catch (const std::exception &) {
        schemaError(std::string{field}, std::string{field} +
                                            " must contain finite numbers");
    }
    const auto length_squared = result.x * result.x + result.y * result.y +
                                result.z * result.z + result.w * result.w;
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z) || !std::isfinite(result.w) ||
        !std::isfinite(length_squared) || length_squared <= 1.0e-12f) {
        schemaError(std::string{field}, std::string{field} +
                                            " must be finite and non-zero");
    }
    return result;
}

phys::Shape parseShape(const Json &value, std::string_view field) {
    requireObject(value, field);
    const auto type = requiredString(value, "type", field);
    const auto center = value.contains("center")
                            ? queryVec3(value.at("center"),
                                        std::string{field} + "/center")
                            : vec3{};
    if (type == "sphere") {
        const auto radius = value.value("radius", 0.5f);
        if (!std::isfinite(radius) || radius < 0.0f) {
            schemaError(std::string{field} + "/radius",
                        "sphere radius must be finite and non-negative");
        }
        return phys::Sphere{center, radius};
    }
    const auto rotation = value.contains("rotation")
                              ? queryQuat(value.at("rotation"),
                                          std::string{field} + "/rotation")
                              : quat{};
    if (type == "box") {
        const auto half_extents = value.contains("half_extents")
                                      ? queryVec3(value.at("half_extents"),
                                                  std::string{field} +
                                                      "/half_extents")
                                      : vec3{0.5f, 0.5f, 0.5f};
        if (half_extents.x < 0.0f || half_extents.y < 0.0f ||
            half_extents.z < 0.0f) {
            schemaError(std::string{field} + "/half_extents",
                        "box half_extents must be non-negative");
        }
        return phys::Box{center, rotation, half_extents};
    }
    if (type == "capsule") {
        const auto half_height = value.value("half_height", 0.5f);
        const auto radius = value.value("radius", 0.5f);
        if (!std::isfinite(half_height) || !std::isfinite(radius) ||
            half_height < 0.0f || radius < 0.0f) {
            schemaError(std::string{field},
                        "capsule dimensions must be finite and non-negative");
        }
        return phys::Capsule{center, rotation, half_height, radius};
    }
    schemaError(std::string{field} + "/type", "unsupported query shape type");
}

std::vector<phys::Collider> collidersFromScene(const OrderedJson &scene) {
    std::vector<phys::Collider> result;
    for (const auto &entry : scene.at("colliders")) {
        const auto &shape = entry.at("shape");
        result.push_back(phys::Collider{
            .id = entry.at("name").get<std::string>(),
            .shape = parseShape(shape, "prepared_projection/collider"),
            .identity = phys::ColliderIdentity{
                .collider_id = phys::ColliderId{entry.at("id").get<std::uint64_t>()},
                .name = entry.at("name").get<std::string>(),
            },
            .metadata = phys::ColliderQueryMetadata{
                .layer = entry.at("layer").get<std::uint32_t>(),
                .mask = entry.at("mask").get<std::uint32_t>(),
                .trigger = entry.at("trigger").get<bool>(),
                .one_way = entry.at("one_way").get<bool>(),
            },
        });
    }
    return result;
}

OrderedJson raycastResult(const OrderedJson &scene, const Json &query,
                          std::string_view context) {
    if (!query.contains("ray") || !query.at("ray").is_object()) {
        schemaError(std::string{context} + "/ray", "raycast query requires ray object");
    }
    const auto &ray_value = query.at("ray");
    const phys::Ray ray{
        .origin = queryVec3(ray_value.at("origin"),
                            std::string{context} + "/ray/origin"),
        .direction = queryVec3(ray_value.at("direction"),
                               std::string{context} + "/ray/direction"),
        .max_distance = ray_value.value("max_distance",
                                        std::numeric_limits<float>::max()),
    };
    const auto direction_length_squared = ray.direction.x * ray.direction.x +
                                          ray.direction.y * ray.direction.y +
                                          ray.direction.z * ray.direction.z;
    if (!std::isfinite(ray.max_distance) || ray.max_distance < 0.0f ||
        !std::isfinite(direction_length_squared) ||
        direction_length_squared <= 1.0e-12f) {
        schemaError(std::string{context} + "/ray",
                    "ray direction must be non-zero and max_distance non-negative");
    }
    auto colliders = collidersFromScene(scene);
    const auto hits = phys::raycastAll(ray, colliders);
    auto data = OrderedJson::array();
    for (const auto &hit : hits) {
        data.push_back({{"name", hit.identity.name},
                        {"collider_id", hit.identity.collider_id.value},
                        {"distance", hit.distance},
                        {"position", OrderedJson::array(
                             {hit.position.x, hit.position.y, hit.position.z})},
                        {"normal", OrderedJson::array(
                             {hit.normal.x, hit.normal.y, hit.normal.z})}});
    }
    return data;
}

OrderedJson overlapResult(const OrderedJson &scene, const Json &query,
                          std::string_view context) {
    if (!query.contains("shape")) {
        schemaError(std::string{context} + "/shape",
                    "overlap query requires shape");
    }
    const auto shape = parseShape(query.at("shape"),
                                  std::string{context} + "/shape");
    auto colliders = collidersFromScene(scene);
    const auto hits = phys::overlapAllHits(shape, colliders);
    auto data = OrderedJson::array();
    for (const auto &hit : hits) {
        data.push_back({{"name", hit.identity.name},
                        {"collider_id", hit.identity.collider_id.value}});
    }
    return data;
}

OrderedJson cameraResult(const OrderedJson &object, const Json &query,
                         std::string_view context) {
    const auto *component = findEvaluatedComponent(object, "camera");
    if (component == nullptr) {
        schemaError(std::string{context} + "/object_id",
                    "camera query object has no camera component");
    }
    const auto decoded = requireComponentCodec("camera").decodeAuthored(*component);
    const auto &camera = std::any_cast<const CameraCodecData &>(decoded);
    const auto &world = object.at("world_trs");
    const auto position = queryVec3(world.at("pos"), "prepared_camera/world/pos");
    const auto rotation = queryQuat(world.at("rotation"),
                                    "prepared_camera/world/rotation");
    const auto orientation = glm::normalize(toGlm(rotation));
    const auto direction = orientation * glm::vec3{0.0f, 0.0f, 1.0f};
    const auto up = orientation * glm::vec3{0.0f, 1.0f, 0.0f};
    const auto width = query.value("width", 1U);
    const auto height = query.value("height", 1U);
    if (width == 0 || height == 0) {
        schemaError(std::string{context}, "camera query dimensions must be non-zero");
    }
    glm::mat4 projection;
    if (camera.projection_kind == CameraProjectionKind::Orthographic) {
        projection = glm::orthoRH_ZO(-camera.xmag, camera.xmag,
                                     -camera.ymag, camera.ymag,
                                     camera.znear, camera.zfar);
    } else {
        const auto aspect = camera.aspect.value_or(
            static_cast<float>(width) / static_cast<float>(height));
        projection = glm::perspectiveRH_ZO(camera.yfov, aspect,
                                           camera.znear, camera.zfar);
    }
    const auto glm_position = toGlm(position);
    const auto view = glm::lookAt(glm_position, glm_position + direction, up);
    return OrderedJson{{"position", vecJson(glm_position)},
                       {"direction", vecJson(direction)},
                       {"up", vecJson(up)},
                       {"view", matrixJson(view)},
                       {"projection", matrixJson(projection)},
                       {"view_projection", matrixJson(projection * view)}};
}

float srgbToLinear(float value) {
    return value <= 0.04045f ? value / 12.92f
                             : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

OrderedJson lightResult(const OrderedJson &component) {
    auto result = component;
    const auto &color = component.at("color");
    result["linear_color"] = OrderedJson::array(
        {srgbToLinear(color.at(0).get<float>()),
         srgbToLinear(color.at(1).get<float>()),
         srgbToLinear(color.at(2).get<float>())});
    return result;
}

} // namespace

std::string_view editorPreviewProjectionErrorCodeName(
    EditorPreviewProjectionErrorCode code) noexcept {
    switch (code) {
    case EditorPreviewProjectionErrorCode::schema_violation:
        return "schema_violation";
    case EditorPreviewProjectionErrorCode::method_unavailable:
        return "method_unavailable";
    }
    return "method_unavailable";
}

EditorPreviewProjectionError::EditorPreviewProjectionError(
    EditorPreviewProjectionErrorCode code, std::string field,
    std::string adapter, std::string message)
    : std::runtime_error{std::move(message)}, code_{code},
      field_{std::move(field)}, adapter_{std::move(adapter)} {}

PreparedProjection prepareEditorPreviewProjection(
    const AuthoringSceneDocument &base_document, const Json &overrides,
    const EditorPreviewProjectionFaultHook &fault_hook) {
    auto raw = base_document.rawJson();
    applyOverrides(raw, base_document, overrides, fault_hook);
    AuthoringSceneDocument staged;
    try {
        if (base_document.revision().value ==
            std::numeric_limits<std::uint64_t>::max()) {
            schemaError("overrides", "prepared projection revision space is exhausted");
        }
        // The unpublished document uses the same candidate revision that a
        // real projection transaction would validate.  It is never published;
        // the live SceneRevision therefore remains unchanged.
        staged = base_document.stage(
            std::move(raw), SceneRevision{base_document.revision().value + 1});
    } catch (const EditorPreviewProjectionError &) {
        throw;
    } catch (const std::exception &error) {
        schemaError("overrides", error.what());
    }
    auto evaluated = evaluatedScene(staged);
    return PreparedProjection{.document = std::move(staged),
                              .evaluated_scene = std::move(evaluated)};
}

OrderedJson EditorPreviewEvaluationContext::evaluate(
    const Json &queries,
    const EditorPreviewProjectionFaultHook &fault_hook) const {
    if (!queries.is_array()) {
        schemaError("queries", "eval_preview queries must be an array");
    }
    auto results = OrderedJson::array();
    for (std::size_t index = 0; index < queries.size(); ++index) {
        if (fault_hook) fault_hook("query", index);
        const auto &query = queries.at(index);
        const auto context = "queries/" + std::to_string(index);
        requireObject(query, context);
        const auto kind = requiredString(query, "kind", context);
        OrderedJson data;
        if (kind == "component") {
            if (!query.contains("object_id")) {
                schemaError(context + "/object_id",
                            "component query requires object_id");
            }
            const auto object = exactUnsigned(query.at("object_id"),
                                              context + "/object_id");
            const auto component_name =
                requiredString(query, "component_slot", context);
            const auto &evaluated = findEvaluatedObject(
                projection_.evaluated_scene, object, context + "/object_id");
            const auto *component =
                findEvaluatedComponent(evaluated, component_name);
            if (component == nullptr) {
                schemaError(context + "/component_slot",
                            "component query target is absent");
            }
            if (findComponentCodec(component_name) == nullptr) {
                unavailable(context + "/component_slot", component_name,
                            "component cannot be evaluated from prepared state");
            }
            data = component_name == "light" ? lightResult(*component) : *component;
        } else if (kind == "descendant_world") {
            if (!query.contains("object_id")) {
                schemaError(context + "/object_id",
                            "descendant_world query requires object_id");
            }
            const auto root_id = exactUnsigned(query.at("object_id"),
                                               context + "/object_id");
            const auto &root = findEvaluatedObject(
                projection_.evaluated_scene, root_id, context + "/object_id");
            const auto &scenes = projection_.evaluated_scene.at("scenes");
            const auto root_scene = std::find_if(
                scenes.begin(), scenes.end(), [root_id](const auto &scene) {
                    return std::any_of(
                        scene.at("objects").begin(), scene.at("objects").end(),
                        [root_id](const auto &object) {
                            return object.at("object_id") == root_id;
                        });
                });
            if (root_scene == scenes.end()) {
                schemaError(context + "/object_id",
                            "descendant_world root scene is absent");
            }
            std::unordered_set<std::string> descendants;
            if (!root.at("name").is_null()) {
                descendants.insert(root.at("name").get<std::string>());
            }
            data = OrderedJson::array();
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto &object : root_scene->at("objects")) {
                    if (object.at("parent").is_string() &&
                        descendants.contains(
                            object.at("parent").get<std::string>()) &&
                        object.at("name").is_string()) {
                        changed |= descendants
                                       .insert(object.at("name")
                                                   .get<std::string>())
                                       .second;
                    }
                }
            }
            for (const auto &object : root_scene->at("objects")) {
                const auto id = object.at("object_id").get<std::uint64_t>();
                const bool include =
                    id == root_id ||
                    (object.at("name").is_string() &&
                     descendants.contains(
                         object.at("name").get<std::string>()));
                if (include) {
                    data.push_back({{"object_id", id},
                                    {"name", object.at("name")},
                                    {"world_trs", object.at("world_trs")}});
                }
            }
        } else if (kind == "raycast") {
            data = raycastResult(projection_.evaluated_scene, query, context);
        } else if (kind == "overlap") {
            data = overlapResult(projection_.evaluated_scene, query, context);
        } else if (kind == "camera") {
            if (!query.contains("object_id")) {
                schemaError(context + "/object_id", "camera query requires object_id");
            }
            const auto object_id = exactUnsigned(query.at("object_id"),
                                                 context + "/object_id");
            data = cameraResult(findEvaluatedObject(
                                    projection_.evaluated_scene, object_id,
                                    context + "/object_id"),
                                query, context);
        } else {
            unavailable(context + "/kind", "query_adapter",
                        "eval_preview query kind is unavailable: " + kind);
        }
        results.push_back({{"kind", kind}, {"data", std::move(data)}});
    }
    return results;
}

} // namespace Pelican
