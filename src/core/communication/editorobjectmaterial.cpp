// MT-2: a scene object's own materials over RPC. set_object_material writes
// the object's simplemodelview.materials rows through the editor journal (one
// edit: CAS on the scene revision, undo / redo, the same projection a
// set_component_value takes); get_object_materials reads what each primitive
// of the object draws with and where that comes from.

#include "editorcommandservice.hpp"
#include "editorjournalinternal.hpp"
#include "rpcserver.hpp"

#include "../asset/model.hpp"
#include "../container.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../loader/scene.hpp"
#include "../material/projectmaterialasset.hpp"
#include "../model/engineprimitives.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#include "../userpublic/components/predefined.hpp"

#include "../../project/jsonrpc.hpp"
#include "../../project/materialformat.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {
namespace {

using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;
using PrimitiveKey = std::pair<std::uint32_t, std::uint32_t>;
using Target = std::optional<PrimitiveKey>;

constexpr std::string_view componentName = "simplemodelview";
constexpr std::string_view objectUnknownCode = "material.object_unknown@1";
constexpr std::string_view modelUnknownCode = "material.model_unknown@1";
constexpr std::string_view nameUnknownCode = "material.name_unknown@1";
constexpr std::string_view bindingInvalidCode = "material.binding_invalid@1";

[[noreturn]] void refuse(std::string_view method, const std::string &message, std::string_view code,
                         Json fields) {
    throw JsonRpcHandlerError(JsonRpcErrorCodes::applicationError, std::string{method} + ": " + message,
                              Json{{"error_code", std::string{code}}, {"error_fields", std::move(fields)}});
}

[[noreturn]] void invalidParams(std::string_view method, const std::string &message) {
    throw std::invalid_argument(std::string{method} + " " + message);
}

void requireOnly(std::string_view method, const Json &params, std::initializer_list<std::string_view> keys) {
    if (!params.is_object()) invalidParams(method, "params must be an object");
    for (const auto &[key, value] : params.items()) {
        (void)value;
        if (std::find(keys.begin(), keys.end(), key) == keys.end())
            invalidParams(method, "does not take '" + key + "'");
    }
}

std::uint64_t requireUnsigned(std::string_view method, const Json &params, const char *key, bool nonzero) {
    const auto found = params.find(key);
    if (found == params.end() || !found->is_number_unsigned() || (nonzero && found->get<std::uint64_t>() == 0))
        invalidParams(method, std::string{"requires a "} + (nonzero ? "positive" : "non-negative") +
                                  " integer '" + key + "'");
    return found->get<std::uint64_t>();
}

PrimitiveKey requirePrimitive(std::string_view method, const Json &value) {
    const auto index = [](const Json &object, const char *key) {
        return object.contains(key) && object.at(key).is_number_integer() && object.at(key).get<std::int64_t>() >= 0 &&
               object.at(key).get<std::int64_t>() <= std::numeric_limits<std::uint32_t>::max();
    };
    if (!value.is_object() || value.size() != 2 || !index(value, "mesh") || !index(value, "primitive"))
        invalidParams(method, "'primitive' must be {mesh, primitive} (non-negative integers)");
    return {value.at("mesh").get<std::uint32_t>(), value.at("primitive").get<std::uint32_t>()};
}

Json targetJson(const Target &target) {
    return target ? Json{{"mesh", target->first}, {"primitive", target->second}} : Json(nullptr);
}

// The object, its scene and its simplemodelview, as the committed document
// holds them.
struct ObjectModel {
    EditorJournalInternal::ObjectLocation object;
    std::size_t component_index = 0;
    Json component;
    std::string model;
};

ObjectModel requireObjectModel(std::string_view method, const AuthoringSceneDocument &document,
                               std::uint64_t object_id, const std::optional<std::string> &scene_id) {
    const auto objects = EditorJournalInternal::objectLocations(document);
    const auto found = std::find_if(objects.begin(), objects.end(),
                                    [&](const auto &object) { return object.object_id.value == object_id; });
    if (found == objects.end())
        refuse(method, "no authored object " + std::to_string(object_id), objectUnknownCode,
               {{"object", object_id}, {"reason", "no authored object has this id"}});
    // MT-2b: an object that is there is named by its authoring name (when it
    // has one) and its scene.
    const auto fields = [&](std::string reason) {
        Json result{{"object", object_id}, {"scene_id", found->scene_id}};
        if (found->name) result["name"] = *found->name;
        result["reason"] = std::move(reason);
        return result;
    };
    if (scene_id && *scene_id != found->scene_id)
        refuse(method, "object " + std::to_string(object_id) + " is in scene '" + found->scene_id + "'",
               objectUnknownCode, fields("the object is in scene '" + found->scene_id + "'"));
    ObjectModel result{.object = *found};
    const auto &components = found->authored.contains("components") ? found->authored.at("components")
                                                                     : Json::array();
    for (std::size_t index = 0; index < components.size(); ++index) {
        const auto &component = components.at(index);
        if (!component.is_object() || component.value("name", std::string{}) != componentName) continue;
        result.component_index = index;
        result.component = component;
        result.model = component.value("model", std::string{});
        return result;
    }
    refuse(method, "object " + std::to_string(object_id) + " draws no model", objectUnknownCode,
           fields("the object has no simplemodelview"));
}

// The model's primitives (with the glTF material each carries) and its
// template, held by the model container.
struct ModelState {
    std::vector<ModelMaterialSlot> slots;
    const ModelTemplate *model = nullptr;
};

ModelState requireModel(std::string_view method, const std::string &model, std::uint64_t object_id) {
    auto *models = FastModuleContainer::tryGet<ModelAssetContainer>();
    if (models == nullptr)
        refuse(method, "this host runs no model container", modelUnknownCode,
               {{"model", model}, {"object", object_id}, {"reason", "no model container"}});
    ModelState state;
    try {
        if (isEnginePrimitiveReference(model)) requireKnownEnginePrimitive(model);
        state.model = &models->getModelTemplateByName(model);
        state.slots = models->materialSlots(model);
    } catch (const std::exception &error) {
        refuse(method, "model '" + model + "': " + error.what(), modelUnknownCode,
               {{"model", model}, {"object", object_id}, {"reason", error.what()}});
    }
    return state;
}

// A material an override can name: one of the model's glTF materials or a
// project material this engine registered.
bool knownMaterial(const ModelTemplate &model, std::string_view name) {
    for (const auto &named : model.named_materials)
        if (named.name == name) return true;
    if (const auto *project = FastModuleContainer::tryGet<ProjectMaterialAssetContainer>())
        for (const auto &named : project->namedMaterials())
            if (named.name == name) return true;
    return false;
}

struct Row {
    std::uint32_t id = 0;
    std::string material;
};

// The object's rows by what they cover (the codec made sure no two cover the
// same).
std::map<Target, Row> currentRows(const Json &component) {
    std::map<Target, Row> rows;
    const auto list = component.find("materials");
    if (list == component.end() || !list->is_object() || !list->contains("rows")) return rows;
    for (const auto &row : list->at("rows")) {
        if (!row.is_object() || !row.contains("$row")) continue;
        Target target;
        if (row.contains("mesh") && row.contains("primitive"))
            target = PrimitiveKey{row.at("mesh").get<std::uint32_t>(), row.at("primitive").get<std::uint32_t>()};
        rows.insert_or_assign(target, Row{.id = row.at("$row").at("id").get<std::uint32_t>(),
                                          .material = row.value("material", std::string{})});
    }
    return rows;
}

Json rowAddress(const ObjectModel &object, std::optional<std::uint32_t> row_id = std::nullopt,
                std::optional<std::string> leaf_path = std::nullopt) {
    Json address{{"authoring_object_id", object.object.object_id.value},
                 {"component",
                  {{"type", std::string{componentName}},
                   {"occurrence", object.component_index},
                   {"origin", {{"kind", "authored"}, {"authoring_component_index", object.component_index}}}}},
                 {"list_path", "/materials"}};
    if (row_id) address["row_id"] = *row_id;
    if (leaf_path) address["leaf_path"] = *leaf_path;
    return address;
}

// The live instance the object draws now, when it has one.
std::optional<ModelInstanceId> liveInstance(AuthoringObjectId object) {
    const auto *loader = FastModuleContainer::tryGet<SceneLoader>();
    auto *ecs = FastModuleContainer::tryGet<ECSCore>();
    if (loader == nullptr || ecs == nullptr) return std::nullopt;
    for (const auto &binding : loader->runtimeObjectBindings()) {
        if (binding.authoring_object_id != object) continue;
        const auto *view = ecs->getTemplatePublicModule().tryComponent<SimpleModelViewComponent>(binding.object_id);
        if (view == nullptr) return std::nullopt;
        return view->model_instance_id;
    }
    return std::nullopt;
}

} // namespace

OrderedJson EditorCommandService::setObjectMaterial(const Json &params) {
    constexpr std::string_view method = "set_object_material";
    requireOnly(method, params,
                {"actor_id", "object", "scene_id", "base_scene_revision", "material", "primitive", "slot", "materials"});
    const auto actor = requireUnsigned(method, params, "actor_id", true);
    const auto object_id = requireUnsigned(method, params, "object", true);
    std::optional<std::string> scene_id;
    if (const auto found = params.find("scene_id"); found != params.end()) {
        if (!found->is_string() || found->get_ref<const std::string &>().empty())
            invalidParams(method, "'scene_id' must be a non-empty string");
        scene_id = found->get<std::string>();
    }
    const bool listed = params.contains("materials");
    if (listed == params.contains("material"))
        invalidParams(method, "takes 'material' (with 'primitive' or 'slot', or neither) or 'materials'");
    if (listed && (params.contains("primitive") || params.contains("slot")))
        invalidParams(method, "'materials' entries name their own primitive");
    if (params.contains("primitive") && params.contains("slot"))
        invalidParams(method, "takes 'primitive' or 'slot', not both");
    const auto materialValue = [&](const Json &value) -> std::optional<std::string> {
        if (value.is_null()) return std::nullopt;
        if (!value.is_string() || value.get_ref<const std::string &>().empty())
            invalidParams(method, "'material' must be a non-empty string or null");
        return value.get<std::string>();
    };

    const auto object = requireObjectModel(method, document(), object_id, scene_id);
    const auto model = requireModel(method, object.model, object_id);

    // What is asked: each target (one primitive, or every primitive) with the
    // material it is to carry (nullopt: the object's own one taken off).
    std::vector<std::pair<Target, std::optional<std::string>>> requested;
    const auto add = [&](Target target, std::optional<std::string> material) {
        if (std::any_of(requested.begin(), requested.end(), [&](const auto &entry) { return entry.first == target; }))
            invalidParams(method, "names the same target twice");
        if (target && std::none_of(model.slots.begin(), model.slots.end(), [&](const ModelMaterialSlot &slot) {
                return slot.mesh == target->first && slot.primitive == target->second;
            }))
            refuse(method,
                   "model '" + object.model + "' has no mesh " + std::to_string(target->first) + " primitive " +
                       std::to_string(target->second),
                   bindingInvalidCode,
                   {{"model", object.model}, {"object", object_id}, {"reason", "no such primitive"}});
        requested.emplace_back(target, std::move(material));
    };
    if (listed) {
        const auto &entries = params.at("materials");
        if (!entries.is_array() || entries.empty()) invalidParams(method, "'materials' must be a non-empty array");
        for (const auto &entry : entries) {
            if (!entry.is_object() || !entry.contains("material"))
                invalidParams(method, "'materials' entries are {material, primitive?}");
            for (const auto &[key, value] : entry.items()) {
                (void)value;
                if (key != "material" && key != "primitive")
                    invalidParams(method, "'materials' entries do not take '" + key + "'");
            }
            add(entry.contains("primitive") && !entry.at("primitive").is_null()
                    ? Target{requirePrimitive(method, entry.at("primitive"))}
                    : Target{},
                materialValue(entry.at("material")));
        }
    } else {
        const auto material = materialValue(params.at("material"));
        if (params.contains("primitive")) {
            add(requirePrimitive(method, params.at("primitive")), material);
        } else if (params.contains("slot")) {
            if (!params.at("slot").is_string()) invalidParams(method, "'slot' must be a string");
            const auto slot = params.at("slot").get<std::string>();
            for (const auto &candidate : model.slots)
                if (candidate.slot == slot) add(PrimitiveKey{candidate.mesh, candidate.primitive}, material);
            if (requested.empty())
                refuse(method, "no primitive of model '" + object.model + "' carries '" + slot + "'",
                       bindingInvalidCode,
                       {{"model", object.model}, {"object", object_id},
                        {"reason", "no primitive carries slot '" + slot + "'"}});
        } else {
            add(std::nullopt, material);
        }
    }
    for (const auto &[target, material] : requested) {
        (void)target;
        if (material && !knownMaterial(*model.model, *material))
            refuse(method, "no material '" + *material + "' to put on object " + std::to_string(object_id),
                   nameUnknownCode,
                   {{"name", *material},
                    {"reason", "neither a registered project material nor a glTF material of model '" +
                                   object.model + "'"}});
    }

    // One journal operation per target that changes: a row's material set
    // in place, a row added, or a row taken off.
    const auto rows = currentRows(object.component);
    auto operations = Json::array();
    auto previous = Json::array();
    for (const auto &[target, material] : requested) {
        const auto row = rows.find(target);
        const std::optional<std::string> before =
            row != rows.end() ? std::optional<std::string>{row->second.material} : std::nullopt;
        Json entry{{"material", before ? Json(*before) : Json(nullptr)}};
        if (target) entry["primitive"] = targetJson(target);
        previous.push_back(std::move(entry));
        if (before == material) continue;
        if (!material) {
            operations.push_back({{"op", "remove_component_field"}, {"address", rowAddress(object, row->second.id)}});
        } else if (row != rows.end()) {
            operations.push_back({{"op", "set_component_value"},
                                  {"address", rowAddress(object, row->second.id, "/material")},
                                  {"value", *material}});
        } else {
            Json value{{"material", *material}};
            if (target) {
                value["mesh"] = target->first;
                value["primitive"] = target->second;
            }
            operations.push_back({{"op", "insert_component_field"},
                                  {"address", rowAddress(object)},
                                  {"position", {{"end", true}}},
                                  {"value", std::move(value)}});
        }
    }

    OrderedJson answer;
    if (operations.empty()) {
        answer = OrderedJson{{"changed", false}};
    } else {
        const auto revision = params.contains("base_scene_revision")
                                  ? requireUnsigned(method, params, "base_scene_revision", false)
                                  : resolved().revision().value;
        answer = edit(Json{{"actor_id", actor}, {"base_revision", revision}, {"operations", std::move(operations)}});
        answer["changed"] = true;
    }
    answer["object"] = object_id;
    answer["scene_id"] = object.object.scene_id;
    answer["model"] = object.model;
    answer["previous"] = OrderedJson{{"materials", OrderedJson::parse(previous.dump())}};
    return answer;
}

OrderedJson EditorCommandService::getObjectMaterials(const Json &params) const {
    constexpr std::string_view method = "get_object_materials";
    requireOnly(method, params, {"object", "scene_id"});
    const auto object_id = requireUnsigned(method, params, "object", true);
    std::optional<std::string> scene_id;
    if (const auto found = params.find("scene_id"); found != params.end()) {
        if (!found->is_string() || found->get_ref<const std::string &>().empty())
            invalidParams(method, "'scene_id' must be a non-empty string");
        scene_id = found->get<std::string>();
    }
    const auto object = requireObjectModel(method, document(), object_id, scene_id);
    const auto model = requireModel(method, object.model, object_id);
    const auto rows = currentRows(object.component);

    // The model's material per primitive: its material bindings, else the
    // glTF material.
    std::map<PrimitiveKey, std::string> bound;
    if (const auto bindings = GET_MODULE(ModelAssetContainer).declaredMaterialBindings(object.model))
        for (const auto &binding : bindings->bindings)
            bound.insert_or_assign(PrimitiveKey{binding.mesh_index, binding.primitive_index}, binding.material);

    std::map<PrimitiveKey, std::string> drawn;
    if (const auto instance = liveInstance(object.object.object_id)) {
        if (const auto *instances = FastModuleContainer::tryGet<PolygonInstanceContainer>())
            for (const auto &entry : instances->instancePrimitiveMaterials(*instance))
                drawn.insert_or_assign(PrimitiveKey{entry.mesh, entry.primitive}, entry.name);
    }

    auto materials = Json::array();
    for (const auto &[target, row] : rows) {
        Json entry{{"row_id", row.id}, {"material", row.material}};
        if (target) entry["primitive"] = targetJson(target);
        entry["resolved"] = knownMaterial(*model.model, row.material);
        materials.push_back(std::move(entry));
    }
    const auto resolvedRow = [&](const Target &target) -> const Row * {
        const auto found = rows.find(target);
        return found != rows.end() && knownMaterial(*model.model, found->second.material) ? &found->second : nullptr;
    };
    auto primitives = Json::array();
    for (const auto &slot : model.slots) {
        const PrimitiveKey key{slot.mesh, slot.primitive};
        const auto found = bound.find(key);
        const auto model_material = found != bound.end() ? found->second : slot.slot;
        const auto *own = resolvedRow(key);
        if (own == nullptr) own = resolvedRow(std::nullopt);
        const auto live = drawn.find(key);
        primitives.push_back({{"mesh", slot.mesh},
                              {"primitive", slot.primitive},
                              {"slot", slot.slot},
                              {"material", own != nullptr ? own->material : model_material},
                              {"source", own != nullptr ? "object" : "model"},
                              {"model_material", model_material},
                              {"drawn", live != drawn.end() ? Json(live->second) : Json(nullptr)}});
    }
    return OrderedJson{{"object", object_id},
                       {"scene_id", object.object.scene_id},
                       {"model", object.model},
                       {"materials", OrderedJson::parse(materials.dump())},
                       {"primitives", OrderedJson::parse(primitives.dump())}};
}

OrderedJson EditorCommandRpcAdapter::setObjectMaterial(const Json &params) const {
    if (!mutable_service_) throw std::logic_error("editor RPC adapter is read-only");
    return mutable_service_->setObjectMaterial(params);
}

OrderedJson EditorCommandRpcAdapter::getObjectMaterials(const Json &params) const {
    return service_.getObjectMaterials(params);
}

} // namespace Pelican
