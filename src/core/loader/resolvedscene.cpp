#include "resolvedscene.hpp"

#include "authoringsceneauthority.hpp"
#include "../userpublic/details/behavior/registerer.hpp"

#include <picosha2.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Pelican {

std::string_view resolvedSceneErrorCodeName(
    ResolvedSceneErrorCode code) noexcept {
    switch (code) {
    case ResolvedSceneErrorCode::generated_component_unsupported:
        return "generated_component_unsupported";
    }
    return "resolved_scene_unknown_error";
}

ResolvedSceneError::ResolvedSceneError(ResolvedSceneErrorCode code,
                                       const std::string &message)
    : std::runtime_error(std::string{resolvedSceneErrorCodeName(code)} +
                         ": " + message),
      code_{code} {}

const ResolvedSceneView *ResolvedScene::findScene(
    std::string_view scene_id) const noexcept {
    const auto found = std::find_if(
        scenes_.begin(), scenes_.end(), [scene_id](const auto &scene) {
            return scene.scene_id == scene_id;
        });
    return found == scenes_.end() ? nullptr : &*found;
}

const ComponentCodecValue &ResolvedComponent::requireRuntimeValue() const {
    if (runtime_resolution_error) {
        throw std::runtime_error(*runtime_resolution_error);
    }
    if (runtime_codec == nullptr || !runtime_value.has_value()) {
        throw std::logic_error("resolved component has no runtime value: " +
                               name);
    }
    return runtime_value;
}

SceneProjectionState::SceneProjectionState(AuthoringSceneDocument authoring,
                                           ResolvedScene resolved)
    : authoring_{std::move(authoring)}, resolved_{std::move(resolved)} {
    if (authoring_.revision().value == 0 ||
        authoring_.revision() != resolved_.revision() ||
        resolved_.resolverGeneration().value == 0) {
        throw std::logic_error(
            "scene projection state requires one non-zero authoring/resolved revision");
    }
}

void SceneProjectionState::swap(SceneProjectionState &other) noexcept {
    authoring_.swap(other.authoring_);
    using std::swap;
    swap(resolved_, other.resolved_);
}

namespace {

void appendLe32(std::vector<unsigned char> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8) {
        bytes.push_back(static_cast<unsigned char>((value >> shift) & 0xffU));
    }
}

void appendLp(std::vector<unsigned char> &bytes, std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("resolver input field name exceeds u32 length");
    }
    appendLe32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void appendResolverFloat(std::vector<unsigned char> &bytes,
                         std::string_view field, float value) {
    appendLp(bytes, field);
    if (std::isnan(value)) {
        throw PrefabError{
            PrefabErrorCode::PrefabDependencyMismatch,
            "resolver input cannot contain NaN: " + std::string{field},
            {.json_pointer = "/resolver_inputs/" + std::string{field}}};
    }
    if (value == 0.0F) value = 0.0F;
    appendLe32(bytes, std::bit_cast<std::uint32_t>(value));
}

void appendResolverOptionalFloat(std::vector<unsigned char> &bytes,
                                 std::string_view field,
                                 const std::optional<float> &value) {
    appendLp(bytes, field);
    bytes.push_back(value ? 0x01U : 0x00U);
    if (!value) return;
    auto scalar = *value;
    if (std::isnan(scalar)) {
        throw PrefabError{
            PrefabErrorCode::PrefabDependencyMismatch,
            "resolver input cannot contain NaN: " + std::string{field},
            {.json_pointer = "/resolver_inputs/" + std::string{field}}};
    }
    if (scalar == 0.0F) scalar = 0.0F;
    appendLe32(bytes, std::bit_cast<std::uint32_t>(scalar));
}

template <class Enum>
void appendResolverEnum(std::vector<unsigned char> &bytes,
                        std::string_view field, Enum value) {
    static_assert(std::is_enum_v<Enum>);
    appendLp(bytes, field);
    appendLe32(bytes, static_cast<std::uint32_t>(value));
}

std::string sha256(std::string_view bytes) {
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

bool requestsGeneratedComponent(const nlohmann::json &component) {
    for (auto field = component.begin(); field != component.end(); ++field) {
        if (prefabGeneratedMarkerRule(field.key(), field.value())) return true;
    }
    return false;
}

ResolvedComponent resolveComponent(
    const nlohmann::json &authored,
    ComponentCodecQueryMetadata source_codec,
    std::size_t authoring_component_index,
    std::size_t merged_component_index,
    ResolvedComponentOrigin origin,
    const SceneComponentProvenance &provenance,
    std::vector<BehaviorReloadSourceParams> &behavior_sources,
    const ResolvedSceneDefaults &defaults) {
    if (!origin.isGenerated() && requestsGeneratedComponent(authored)) {
        ResolvedSceneResolver::resolveGeneratedComponent();
    }

    ResolvedComponent result{
        .authoring_component_index = authoring_component_index,
        .merged_component_index = merged_component_index,
        .origin = std::move(origin),
        .name = authored.at("name").get<std::string>(),
        .source_json_exact = authored,
        .effective_json = authored,
        .codec = source_codec,
    };

    if (result.name == "behavior") {
        const auto stable_name = authored.value("type", std::string{});
        const auto params_it = authored.find("params");
        const auto source_params =
            params_it == authored.end() ? nlohmann::json::object() : *params_it;
        behavior_sources.push_back(BehaviorReloadSourceParams{
            .stable_name = stable_name,
            .source_params = source_params,
            .provenance = provenance,
        });

        const auto *registration =
            internal::getBehaviorRegisterer().findByName(stable_name);
        if (registration == nullptr) {
            result.behavior_pending = true;
            result.codec = {ComponentCodecState::Missing, false, {}};
            return result;
        }

        try {
            const auto canonical =
                registration->canonicalize_params(source_params);
            result.behavior_canonical_params = canonical;
            // This is intentionally the parent RPC representation: the pre-WP360
            // query completed behavior params before serializing authored_json.
            result.source_json_exact["params"] =
                nlohmann::json::parse(canonical);
            result.effective_json = result.source_json_exact;
            result.codec = {ComponentCodecState::Registered, true,
                            std::string_view{"behavior_params"}};
        } catch (const std::exception &error) {
            result.runtime_resolution_error = error.what();
            result.codec = {ComponentCodecState::Registered, true,
                            std::string_view{"behavior_params"}};
        }
        return result;
    }

    const auto *codec = findComponentCodec(result.name);
    if (codec == nullptr) {
        // External ECS components retain identity. Their component registrar
        // remains the only decoder; no core default is invented here.
        return result;
    }

    result.runtime_codec = codec;
    try {
        result.runtime_value = codec->decodeAuthored(authored);
        if (result.name == "camera") {
            auto data = std::any_cast<CameraCodecData>(result.runtime_value);
            const auto nested = [&](std::string_view name)
                -> const nlohmann::json * {
                const auto found = authored.find(name);
                return found != authored.end() && found->is_object()
                           ? &*found
                           : nullptr;
            };
            if (!data.projection_specified) {
                data.projection_kind = defaults.camera_projection.kind;
                data.yfov = defaults.camera_projection.yfov;
                data.znear = defaults.camera_projection.znear;
                data.zfar = defaults.camera_projection.zfar;
                data.aspect = defaults.camera_projection.aspect;
                data.xmag = defaults.camera_projection.xmag;
                data.ymag = defaults.camera_projection.ymag;
            } else if (data.projection_kind ==
                       CameraProjectionKind::Perspective) {
                if (!data.yfov) {
                    if (defaults.camera_projection.kind !=
                        CameraProjectionKind::Perspective) {
                        throw std::runtime_error(
                            "camera.yfov is required when changing projection to perspective");
                    }
                    data.yfov = defaults.camera_projection.yfov;
                }
                if (!data.znear) {
                    data.znear = defaults.camera_projection.znear;
                }
                if (!data.zfar) {
                    data.zfar = defaults.camera_projection.zfar;
                }
                // The authored camera contract deliberately clears a fallback
                // perspective aspect when the field is omitted.
                // aspect is intentionally not inherited when it is omitted.
            } else {
                if (!data.xmag) {
                    if (defaults.camera_projection.kind !=
                        CameraProjectionKind::Orthographic) {
                        throw std::runtime_error(
                            "camera.xmag is required when changing projection to orthographic");
                    }
                    data.xmag = defaults.camera_projection.xmag;
                }
                if (!data.ymag) {
                    if (defaults.camera_projection.kind !=
                        CameraProjectionKind::Orthographic) {
                        throw std::runtime_error(
                            "camera.ymag is required when changing projection to orthographic");
                    }
                    data.ymag = defaults.camera_projection.ymag;
                }
                if (!data.znear) {
                    data.znear = defaults.camera_projection.znear;
                }
                if (!data.zfar) {
                    data.zfar = defaults.camera_projection.zfar;
                }
                data.aspect.reset();
            }
            // Keep the typed projection total for downstream consumers. The
            // inactive-kind magnitudes/FOV are carried only as resolver state;
            // encodeCanonical emits fields for the active kind alone.
            if (!data.yfov) data.yfov = defaults.camera_projection.yfov;
            if (!data.xmag) data.xmag = defaults.camera_projection.xmag;
            if (!data.ymag) data.ymag = defaults.camera_projection.ymag;
            data.projection_specified = true;
            const bool projection_complete = data.znear && data.zfar &&
                (data.projection_kind == CameraProjectionKind::Perspective
                     ? static_cast<bool>(data.yfov)
                     : static_cast<bool>(data.xmag) &&
                           static_cast<bool>(data.ymag));
            if (!projection_complete) {
                throw std::logic_error(
                    "resolved camera is missing projection values");
            }
            if (*data.zfar <= *data.znear) {
                throw std::runtime_error(
                    "camera.zfar must be greater than the resolved znear");
            }

            const auto sprite = nested("sprite");
            if (sprite == nullptr || !sprite->contains("pixel_perfect")) {
                data.pixel_perfect = defaults.camera_sprite.pixel_perfect;
            }
            if (sprite == nullptr || !sprite->contains("sort")) {
                data.sprite_sort = defaults.camera_sprite.sort;
            }
            data.sprite_specified = true;
            result.runtime_value = std::move(data);
        }
        result.effective_json = codec->encodeCanonical(result.runtime_value);
    } catch (const ResolvedSceneError &) {
        throw;
    } catch (const std::exception &error) {
        // Preserve the pre-WP360 validation boundary: authoring/query/save may
        // retain an invalid component in an inactive scene. Runtime binders
        // consume this named result and throw it without a second decode.
        result.runtime_value.reset();
        result.runtime_resolution_error = error.what();
        result.effective_json = authored;
    }
    return result;
}

ResolvedSceneDefaults cameraDefaultsAfter(
    const ResolvedComponent &component, ResolvedSceneDefaults defaults) {
    if (component.name != "camera" || component.runtime_codec == nullptr ||
        component.runtime_resolution_error) {
        return defaults;
    }
    const auto &data =
        std::any_cast<const CameraCodecData &>(component.requireRuntimeValue());
    defaults.camera_projection = CameraProjectionSpec{
        .kind = data.projection_kind,
        .yfov = *data.yfov,
        .znear = *data.znear,
        .zfar = *data.zfar,
        .aspect = data.aspect,
        .xmag = *data.xmag,
        .ymag = *data.ymag,
    };
    defaults.camera_sprite = CameraSpritePolicySpec{
        .pixel_perfect = data.pixel_perfect,
        .sort = data.sprite_sort,
    };
    return defaults;
}

struct PendingComponent {
    nlohmann::json json;
    ComponentCodecQueryMetadata codec;
    std::optional<std::size_t> authored_index;
    std::optional<GeneratedComponentOrigin> generated;
};

struct PendingObject {
    const AuthoringObjectView *source = nullptr;
    const PrefabRegistryRecord *prefab_record = nullptr;
    std::optional<PrefabInstanceDeclaration> prefab;
    std::vector<PendingComponent> components;
};

struct PendingScene {
    const AuthoringSceneView *source = nullptr;
    std::vector<PendingObject> objects;
};

} // namespace

bool PrefabResolutionClosure::sameWireIdentity(
    const PrefabResolutionClosure &other) const noexcept {
    return scenes == other.scenes && prefabs == other.prefabs &&
           behaviors == other.behaviors &&
           provider_fingerprint_sha256 == other.provider_fingerprint_sha256 &&
           resolver_inputs_fingerprint_sha256 ==
               other.resolver_inputs_fingerprint_sha256;
}

std::string_view originTransitionErrorCodeName(
    OriginTransitionErrorCode code) noexcept {
    switch (code) {
    case OriginTransitionErrorCode::TargetMismatch:
        return "origin_transition_target_mismatch@1";
    case OriginTransitionErrorCode::GeneratedIdSetMismatch:
        return "origin_transition_gid_set_mismatch@1";
    case OriginTransitionErrorCode::AuthoredIndexSetMismatch:
        return "origin_transition_index_set_mismatch@1";
    case OriginTransitionErrorCode::AuthoredIndexInvalid:
        return "origin_transition_index_invalid@1";
    case OriginTransitionErrorCode::ComponentMismatch:
        return "origin_transition_component_mismatch@1";
    case OriginTransitionErrorCode::ComponentKeyMismatch:
        return "origin_transition_component_key_mismatch@1";
    case OriginTransitionErrorCode::GeneratedIdMismatch:
        return "origin_transition_gid_derivation_mismatch@1";
    case OriginTransitionErrorCode::AuthorityBodyMismatch:
        return "origin_transition_body_mismatch@1";
    }
    return "origin_transition_component_mismatch@1";
}

OriginTransitionError::OriginTransitionError(
    OriginTransitionErrorCode code, std::string message)
    : std::runtime_error(std::string{originTransitionErrorCodeName(code)} +
                         ": " + message),
      code_{code} {}

namespace {

[[noreturn]] void transitionFailure(OriginTransitionErrorCode code,
                                    std::string message) {
    throw OriginTransitionError{code, std::move(message)};
}

const ResolvedObject *transitionObject(const ResolvedScene &resolved,
                                       std::string_view scene_id,
                                       AuthoringObjectId object_id) {
    const auto *scene = resolved.findScene(scene_id);
    if (scene == nullptr) return nullptr;
    const auto found = std::find_if(
        scene->objects.begin(), scene->objects.end(),
        [object_id](const ResolvedObject &object) {
            return object.authoring_object_id == object_id;
        });
    return found == scene->objects.end() ? nullptr : &*found;
}

const AuthoringObjectView *transitionAuthoredObject(
    const AuthoringSceneDocument &document, std::string_view scene_id,
    AuthoringObjectId object_id,
    std::vector<AuthoringSceneView> &authoring_scenes) {
    authoring_scenes = AuthoringSceneAuthority::query(document);
    const auto scene = std::find_if(
        authoring_scenes.begin(), authoring_scenes.end(),
        [scene_id](const AuthoringSceneView &candidate) {
            return candidate.scene_id == scene_id;
        });
    if (scene == authoring_scenes.end()) return nullptr;
    const auto object = std::find_if(
        scene->objects.begin(), scene->objects.end(),
        [object_id](const AuthoringObjectView &candidate) {
            return candidate.authoring_object_id == object_id;
        });
    return object == scene->objects.end() ? nullptr : &*object;
}

const OriginTransitionAuthority *transitionAuthority(
    std::span<const OriginTransitionAuthority> authority,
    std::string_view component_key) {
    const auto found = std::find_if(
        authority.begin(), authority.end(),
        [component_key](const OriginTransitionAuthority &candidate) {
            return candidate.component_key == component_key;
        });
    return found == authority.end() ? nullptr : &*found;
}

const ResolvedComponent *transitionAuthoredComponent(
    const ResolvedObject &object, std::size_t authored_index) {
    const auto found = std::find_if(
        object.components.begin(), object.components.end(),
        [authored_index](const ResolvedComponent &component) {
            const auto *origin = component.origin.authored();
            return origin != nullptr &&
                   origin->authoring_component_index == authored_index;
        });
    return found == object.components.end() ? nullptr : &*found;
}

std::string transitionStableName(const ResolvedComponent &component) {
    return component.name == "behavior"
               ? component.effective_json.value("type", std::string{})
               : std::string{};
}

std::uint64_t transitionUnsigned(const nlohmann::json &value,
                                 std::string_view field) {
    if (value.is_number_unsigned()) return value.get<std::uint64_t>();
    if (value.is_number_integer()) {
        const auto result = value.get<std::int64_t>();
        if (result >= 0) return static_cast<std::uint64_t>(result);
    }
    throw std::invalid_argument("origin transition " + std::string{field} +
                                " must be a non-negative integer");
}

} // namespace

void validateOriginTransitionMap(
    const OriginTransitionMap &map, const ResolvedScene &generated_scene,
    const AuthoringSceneDocument &authored_document,
    const ResolvedScene &authored_scene, std::string_view scene_id,
    AuthoringObjectId object_id, std::string_view instance_id,
    std::span<const OriginTransitionAuthority> authority) {
    const auto *generated_object =
        transitionObject(generated_scene, scene_id, object_id);
    const auto *authored_resolved_object =
        transitionObject(authored_scene, scene_id, object_id);
    std::vector<AuthoringSceneView> authoring_scenes;
    const auto *authored_object = transitionAuthoredObject(
        authored_document, scene_id, object_id, authoring_scenes);
    if (generated_object == nullptr || authored_resolved_object == nullptr ||
        authored_object == nullptr || !generated_object->prefab_instance ||
        generated_object->prefab_instance->instance_id != instance_id ||
        authored_resolved_object->prefab_instance) {
        transitionFailure(OriginTransitionErrorCode::TargetMismatch,
                          "the two snapshots do not describe the requested unpack target");
    }

    std::vector<const ResolvedComponent *> generated_components;
    std::unordered_set<std::string> expected_gids;
    for (const auto &component : generated_object->components) {
        const auto *origin = component.origin.generated();
        if (origin == nullptr || origin->instance_id != instance_id) continue;
        generated_components.push_back(&component);
        expected_gids.insert(origin->stable_generated_id);
    }
    if (generated_components.size() != authority.size() ||
        map.entries.size() != generated_components.size()) {
        transitionFailure(OriginTransitionErrorCode::GeneratedIdSetMismatch,
                          "entry count does not match generated components");
    }

    std::unordered_set<std::string> authority_keys;
    for (const auto &item : authority) {
        if (!authority_keys.insert(item.component_key).second) {
            transitionFailure(OriginTransitionErrorCode::ComponentKeyMismatch,
                              "authority component keys are not unique");
        }
    }

    const auto &authored_components = authored_object->authoredJson().at("components");
    if (!authored_components.is_array() ||
        authored_components.size() < generated_components.size()) {
        transitionFailure(OriginTransitionErrorCode::AuthoredIndexInvalid,
                          "authored component interval is too small");
    }
    const auto first_expanded =
        authored_components.size() - generated_components.size();
    std::unordered_set<std::size_t> expected_indices;
    for (std::size_t index = first_expanded;
         index < authored_components.size(); ++index) {
        expected_indices.insert(index);
        if (transitionAuthoredComponent(*authored_resolved_object, index) ==
            nullptr) {
            transitionFailure(OriginTransitionErrorCode::AuthoredIndexInvalid,
                              "an expanded authored index is absent from next_resolved");
        }
    }

    std::unordered_set<std::string> actual_gids;
    std::unordered_set<std::size_t> actual_indices;
    for (const auto &entry : map.entries) {
        if (entry.scene_id != scene_id ||
            entry.authoring_object_id != object_id) {
            transitionFailure(OriginTransitionErrorCode::TargetMismatch,
                              "an entry names a different scene or object");
        }
        if (!actual_gids.insert(entry.gid).second) {
            transitionFailure(OriginTransitionErrorCode::GeneratedIdSetMismatch,
                              "generated ids are not unique");
        }
        if (entry.final_authored_index >= authored_components.size() ||
            !actual_indices.insert(entry.final_authored_index).second) {
            transitionFailure(OriginTransitionErrorCode::AuthoredIndexInvalid,
                              "authored indices are duplicated or out of range");
        }
    }
    if (actual_gids != expected_gids) {
        transitionFailure(OriginTransitionErrorCode::GeneratedIdSetMismatch,
                          "generated id set is not a bijection");
    }
    if (actual_indices != expected_indices) {
        transitionFailure(OriginTransitionErrorCode::AuthoredIndexSetMismatch,
                          "authored index set is not the expanded interval");
    }

    for (const auto &entry : map.entries) {
        const auto source = std::find_if(
            generated_components.begin(), generated_components.end(),
            [&](const ResolvedComponent *component) {
                return component->origin.generated()->stable_generated_id ==
                       entry.gid;
            });
        if (source == generated_components.end()) {
            transitionFailure(OriginTransitionErrorCode::GeneratedIdSetMismatch,
                              "entry source is absent");
        }
        const auto *generated = (*source)->origin.generated();
        if (entry.component_key != generated->component_key) {
            transitionFailure(OriginTransitionErrorCode::ComponentKeyMismatch,
                              "component_key is not bound to its generated source");
        }
        if (entry.gid != prefabGeneratedId(instance_id, entry.component_key)) {
            transitionFailure(OriginTransitionErrorCode::GeneratedIdMismatch,
                              "gid does not match instance_id and component_key");
        }
        const auto *expected_body =
            transitionAuthority(authority, entry.component_key);
        if (expected_body == nullptr) {
            transitionFailure(OriginTransitionErrorCode::ComponentKeyMismatch,
                              "component_key is absent from the authority bodies");
        }
        const auto *next_component = transitionAuthoredComponent(
            *authored_resolved_object, entry.final_authored_index);
        if (next_component == nullptr ||
            next_component->name != (*source)->name ||
            transitionStableName(*next_component) !=
                transitionStableName(**source)) {
            transitionFailure(OriginTransitionErrorCode::ComponentMismatch,
                              "component kind or behavior stable name changed");
        }
        if (nlohmann::ordered_json(
                authored_components.at(entry.final_authored_index)) !=
            expected_body->body) {
            transitionFailure(OriginTransitionErrorCode::AuthorityBodyMismatch,
                              "final component was not produced from its authority body");
        }
    }
}

OriginTransitionMap buildOriginTransitionMap(
    const ResolvedScene &generated_scene,
    const AuthoringSceneDocument &authored_document,
    const ResolvedScene &authored_scene, std::string_view scene_id,
    AuthoringObjectId object_id, std::string_view instance_id,
    std::span<const OriginTransitionAuthority> authority) {
    const auto *generated_object =
        transitionObject(generated_scene, scene_id, object_id);
    const auto *authored_resolved_object =
        transitionObject(authored_scene, scene_id, object_id);
    std::vector<AuthoringSceneView> authoring_scenes;
    const auto *authored_object = transitionAuthoredObject(
        authored_document, scene_id, object_id, authoring_scenes);
    if (generated_object == nullptr || authored_resolved_object == nullptr ||
        authored_object == nullptr) {
        transitionFailure(OriginTransitionErrorCode::TargetMismatch,
                          "unpack target is absent from a snapshot");
    }
    std::vector<const ResolvedComponent *> generated_components;
    std::size_t base_authored_count = 0;
    for (const auto &component : generated_object->components) {
        if (component.origin.authored() != nullptr) ++base_authored_count;
        const auto *origin = component.origin.generated();
        if (origin != nullptr && origin->instance_id == instance_id) {
            generated_components.push_back(&component);
        }
    }
    std::vector<std::size_t> final_indices;
    for (const auto &component : authored_resolved_object->components) {
        const auto *origin = component.origin.authored();
        if (origin != nullptr &&
            origin->authoring_component_index >= base_authored_count) {
            final_indices.push_back(origin->authoring_component_index);
        }
    }
    std::sort(final_indices.begin(), final_indices.end());
    if (final_indices.size() != generated_components.size()) {
        transitionFailure(OriginTransitionErrorCode::AuthoredIndexInvalid,
                          "next_resolved does not contain the expanded interval");
    }
    OriginTransitionMap result;
    result.entries.reserve(generated_components.size());
    for (std::size_t index = 0; index < generated_components.size(); ++index) {
        const auto *origin = generated_components[index]->origin.generated();
        result.entries.push_back({
            .scene_id = std::string{scene_id},
            .authoring_object_id = object_id,
            .gid = origin->stable_generated_id,
            .component_key = origin->component_key,
            .final_authored_index = final_indices[index],
        });
    }
    validateOriginTransitionMap(result, generated_scene, authored_document,
                                authored_scene, scene_id, object_id,
                                instance_id, authority);
    return result;
}

OriginTransitionMap invertOriginTransitionMap(const OriginTransitionMap &map) {
    auto result = map;
    result.direction =
        map.direction == OriginTransitionDirection::GeneratedToAuthored
            ? OriginTransitionDirection::AuthoredToGenerated
            : OriginTransitionDirection::GeneratedToAuthored;
    return result;
}

nlohmann::ordered_json originTransitionMapJson(const OriginTransitionMap &map) {
    nlohmann::ordered_json entries = nlohmann::ordered_json::array();
    for (const auto &entry : map.entries) {
        entries.push_back({{"scene_id", entry.scene_id},
                           {"authoring_object_id",
                            entry.authoring_object_id.value},
                           {"gid", entry.gid},
                           {"component_key", entry.component_key},
                           {"final_authored_index",
                            entry.final_authored_index}});
    }
    return {{"direction",
             map.direction == OriginTransitionDirection::GeneratedToAuthored
                 ? "generated_to_authored"
                 : "authored_to_generated"},
            {"entries", std::move(entries)}};
}

OriginTransitionMap originTransitionMapFromJson(const nlohmann::json &value) {
    if (!value.is_object() || value.size() != 2U ||
        !value.contains("direction") || !value.at("direction").is_string() ||
        !value.contains("entries") || !value.at("entries").is_array()) {
        throw std::invalid_argument("origin transition map has an invalid shape");
    }
    const auto direction = value.at("direction").get<std::string>();
    OriginTransitionMap result;
    if (direction == "generated_to_authored") {
        result.direction = OriginTransitionDirection::GeneratedToAuthored;
    } else if (direction == "authored_to_generated") {
        result.direction = OriginTransitionDirection::AuthoredToGenerated;
    } else {
        throw std::invalid_argument("origin transition direction is invalid");
    }
    result.entries.reserve(value.at("entries").size());
    for (const auto &raw : value.at("entries")) {
        if (!raw.is_object() || raw.size() != 5U ||
            !raw.contains("scene_id") || !raw.at("scene_id").is_string() ||
            !raw.contains("authoring_object_id") || !raw.contains("gid") ||
            !raw.at("gid").is_string() || !raw.contains("component_key") ||
            !raw.at("component_key").is_string() ||
            !raw.contains("final_authored_index")) {
            throw std::invalid_argument(
                "origin transition entry has an invalid shape");
        }
        const auto object_id = transitionUnsigned(
            raw.at("authoring_object_id"), "authoring_object_id");
        const auto authored_index = transitionUnsigned(
            raw.at("final_authored_index"), "final_authored_index");
        if (authored_index > std::numeric_limits<std::size_t>::max()) {
            throw std::invalid_argument(
                "origin transition authored index is out of range");
        }
        result.entries.push_back({
            .scene_id = raw.at("scene_id").get<std::string>(),
            .authoring_object_id = AuthoringObjectId{object_id},
            .gid = raw.at("gid").get<std::string>(),
            .component_key = raw.at("component_key").get<std::string>(),
            .final_authored_index = static_cast<std::size_t>(authored_index),
        });
    }
    return result;
}

std::string resolverInputsFingerprint(
    const ResolverInputSnapshotV1 &snapshot) {
    // These exact-size structured bindings are the compile-time mirror guard:
    // adding a member anywhere in the input aggregates requires updating the
    // canonical encoder in this same change.
    const auto &[projection, sprite] = snapshot;
    const auto &[kind, yfov, znear, zfar, aspect, xmag, ymag] = projection;
    const auto &[pixel_perfect, sort] = sprite;

    std::vector<unsigned char> bytes;
    appendLp(bytes, "pelican.resolver.inputs.v1");
    appendResolverEnum(bytes, "camera_projection.kind", kind);
    appendResolverFloat(bytes, "camera_projection.yfov", yfov);
    appendResolverFloat(bytes, "camera_projection.znear", znear);
    appendResolverFloat(bytes, "camera_projection.zfar", zfar);
    appendResolverOptionalFloat(bytes, "camera_projection.aspect", aspect);
    appendResolverFloat(bytes, "camera_projection.xmag", xmag);
    appendResolverFloat(bytes, "camera_projection.ymag", ymag);
    appendResolverEnum(bytes, "camera_sprite.pixel_perfect", pixel_perfect);
    appendResolverEnum(bytes, "camera_sprite.sort", sort);
    return picosha2::hash256_hex_string(bytes.begin(), bytes.end());
}

ResolvedScene ResolvedSceneResolver::resolve(
    const AuthoringSceneDocument &document,
    SceneResolverGeneration generation, ResolvedSceneDefaults defaults,
    PrefabRegistrySnapshot registry, BindableProviderSnapshot provider) {
    if (generation.value == 0) {
        throw std::invalid_argument(
            "SceneResolverGeneration must be non-zero");
    }

    // Acquiring the raw view here is intentional and type-visible: this is one
    // of the two authorized targets. All runtime readers consume the result.
    const auto raw_view = AuthoringSceneAuthority::rawView(document);
    (void)raw_view;
    const auto authoring_scenes = AuthoringSceneAuthority::query(document);

    if (document.usesPrefabs() && provider.generation() == 0) {
        throw PrefabError{
            PrefabErrorCode::PrefabProviderStale,
            "prefab resolution requires an explicit non-zero provider snapshot"};
    }

    // Phase 1a: validate every instance and resolve parameter values. No
    // component codec is consulted in this phase.
    std::vector<PendingScene> pending_scenes;
    pending_scenes.reserve(authoring_scenes.size());
    for (const auto &source_scene : authoring_scenes) {
        PendingScene pending{.source = &source_scene};
        pending.objects.reserve(source_scene.objects.size());
        std::unordered_set<std::string> instance_ids;
        for (const auto &source_object : source_scene.objects) {
            PendingObject object{.source = &source_object};
            const auto &authored_object = source_object.authoredJson();
            if (const auto prefab = authored_object.find("prefab");
                prefab != authored_object.end()) {
                object.prefab = resolvePrefabInstance(
                    *prefab, registry,
                    {.scene_id = source_scene.scene_id,
                     .json_pointer = "/prefab"});
                if (!instance_ids.insert(object.prefab->instance_id).second) {
                    throw PrefabError{
                        PrefabErrorCode::PrefabInstanceIdCollision,
                        "duplicate prefab instance_id in scene: " +
                            object.prefab->instance_id,
                        {.scene_id = source_scene.scene_id,
                         .instance_id = object.prefab->instance_id,
                         .prefab = object.prefab->ref,
                         .json_pointer = "/prefab/instance_id"}};
                }
                object.prefab_record = registry.find(object.prefab->ref);
                if (object.prefab_record == nullptr) {
                    throw std::logic_error("resolved prefab disappeared from immutable registry");
                }
            }
            pending.objects.push_back(std::move(object));
        }
        pending_scenes.push_back(std::move(pending));
    }

    // Phase 1b: provider validation precedes substitution, and concrete JSON
    // is the only representation admitted to the codec phase.
    for (auto &scene : pending_scenes) {
        for (auto &object : scene.objects) {
            if (!object.prefab) continue;
            for (const auto &component : object.prefab_record->document->components) {
                auto concrete = resolvePrefabComponentBody(
                    *object.prefab_record->document, component, *object.prefab,
                    provider.descriptors(),
                    {.scene_id = scene.source->scene_id});
                object.components.push_back(PendingComponent{
                    .json = std::move(concrete),
                    .codec = componentCodecQueryMetadata(component.name),
                    .generated = GeneratedComponentOrigin{
                        .stable_generated_id = prefabGeneratedId(
                            object.prefab->instance_id, component.key),
                        .prefab = object.prefab->ref,
                        .instance_id = object.prefab->instance_id,
                        .component_key = component.key,
                        .digest_sha256 = object.prefab_record->digest_sha256,
                    },
                });
            }
        }
    }

    // Phase 2: materialize authored-first/generated-second arrays and enforce
    // the final merged composition rules. GID uniqueness is a scene-local
    // invariant and is checked before any runtime decode.
    for (auto &scene : pending_scenes) {
        std::unordered_set<std::string> generated_ids;
        for (auto &object : scene.objects) {
            std::vector<PendingComponent> generated = std::move(object.components);
            object.components.clear();
            object.components.reserve(object.source->components.size() + generated.size());
            std::size_t transform_count = 0;
            for (const auto &component : object.source->components) {
                const auto &json = component.authoredJson();
                if (json.value("name", std::string{}) == "transform") ++transform_count;
                object.components.push_back(PendingComponent{
                    .json = json,
                    .codec = component.codec,
                    .authored_index = component.declaration_index,
                });
            }
            if (object.prefab) {
                if (transform_count == 0) {
                    throw PrefabError{
                        PrefabErrorCode::PrefabInstanceTransformMissing,
                        "prefab instance requires exactly one authored transform",
                        {.scene_id = scene.source->scene_id,
                         .instance_id = object.prefab->instance_id,
                         .prefab = object.prefab->ref}};
                }
                if (transform_count > 1) {
                    throw PrefabError{
                        PrefabErrorCode::PrefabInstanceTransformDuplicate,
                        "prefab instance has more than one authored transform",
                        {.scene_id = scene.source->scene_id,
                         .instance_id = object.prefab->instance_id,
                         .prefab = object.prefab->ref}};
                }
                std::unordered_set<std::string> component_names;
                for (const auto &component : object.components) {
                    const auto name = component.json.value("name", std::string{});
                    if (name != "behavior" && !component_names.insert(name).second) {
                        throw PrefabError{
                            PrefabErrorCode::PrefabComponentDuplicate,
                            "duplicate non-behavior component in merged prefab object: " + name,
                            {.scene_id = scene.source->scene_id,
                             .instance_id = object.prefab->instance_id,
                             .prefab = object.prefab->ref}};
                    }
                }
                for (const auto &component : generated) {
                    const auto name = component.json.value("name", std::string{});
                    if (name != "behavior" && !component_names.insert(name).second) {
                        throw PrefabError{
                            PrefabErrorCode::PrefabComponentDuplicate,
                            "duplicate non-behavior component in merged prefab object: " + name,
                            {.scene_id = scene.source->scene_id,
                             .instance_id = object.prefab->instance_id,
                             .prefab = object.prefab->ref}};
                    }
                    if (!generated_ids.insert(component.generated->stable_generated_id).second) {
                        throw PrefabError{
                            PrefabErrorCode::PrefabGeneratedIdCollision,
                            "generated component id collision: " +
                                component.generated->stable_generated_id,
                            {.scene_id = scene.source->scene_id,
                             .instance_id = object.prefab->instance_id,
                             .prefab = object.prefab->ref}};
                    }
                }
            }
            object.components.insert(object.components.end(),
                                     std::make_move_iterator(generated.begin()),
                                     std::make_move_iterator(generated.end()));
        }
    }

    // Phase 3: freeze object identity, then resolve object parameters and
    // required component sets against the fully materialized scene.
    for (const auto &scene : pending_scenes) {
        std::unordered_map<std::string, const PendingObject *> objects;
        for (const auto &object : scene.objects) {
            if (object.source->name) objects.emplace(*object.source->name, &object);
        }
        for (const auto &object : scene.objects) {
            if (!object.prefab) continue;
            for (const auto &parameter : object.prefab->parameters) {
                if (parameter.kind != PrefabParameterKind::Object) continue;
                const auto target_name = parameter.value_resolved.get<std::string>();
                const auto target = objects.find(target_name);
                if (target == objects.end()) {
                    throw PrefabError{
                        PrefabErrorCode::PrefabObjectRefUnresolved,
                        "prefab object parameter target does not exist: " + target_name,
                        {.scene_id = scene.source->scene_id,
                         .instance_id = object.prefab->instance_id,
                         .prefab = object.prefab->ref,
                         .parameter = parameter.name}};
                }
                for (const auto &required : parameter.required_components) {
                    const auto present = std::any_of(
                        target->second->components.begin(), target->second->components.end(),
                        [&](const auto &component) {
                            return component.json.value("name", std::string{}) == required;
                        });
                    if (!present) {
                        throw PrefabError{
                            PrefabErrorCode::PrefabObjectRefMissingComponent,
                            "prefab object parameter target lacks required component: " + required,
                            {.scene_id = scene.source->scene_id,
                             .instance_id = object.prefab->instance_id,
                             .prefab = object.prefab->ref,
                             .parameter = parameter.name}};
                    }
                }
            }
        }
    }

    ResolvedScene result;
    result.revision_ = document.revision();
    result.resolver_generation_ = generation;
    result.defaults_ = defaults;
    result.prefab_registry_ = std::move(registry);
    result.bindable_provider_ = std::move(provider);
    result.warnings_.assign(document.warnings().begin(),
                            document.warnings().end());
    result.scenes_.reserve(authoring_scenes.size());

    // Phase 4: canonicalize in the legacy authored-first declaration order.
    // The first resolved camera continues to seed later camera defaults.
    for (const auto &pending_scene : pending_scenes) {
        const auto &source_scene = *pending_scene.source;
        ResolvedSceneView scene{.scene_id = source_scene.scene_id};
        auto scene_camera_defaults = defaults;
        bool first_camera_resolved = false;
        scene.objects.reserve(source_scene.objects.size());
        for (std::size_t object_index = 0;
             object_index < pending_scene.objects.size(); ++object_index) {
            const auto &pending_object = pending_scene.objects[object_index];
            const auto &source_object = *pending_object.source;
            ResolvedObject object{
                .authoring_object_id = source_object.authoring_object_id,
                .authoring_object_index = source_object.declaration_index,
                .name = source_object.name,
                .parent = source_object.parent,
            };
            if (pending_object.prefab) {
                ResolvedPrefabInstance instance{
                    .ref = pending_object.prefab->ref,
                    .instance_id = pending_object.prefab->instance_id,
                    .closure_generation = generation.value,
                    .provider_generation = result.bindable_provider_.generation(),
                };
                instance.parameters.reserve(pending_object.prefab->parameters.size());
                for (const auto &parameter : pending_object.prefab->parameters) {
                    instance.parameters.push_back({
                        .name = parameter.name,
                        .value_resolved = parameter.value_resolved,
                        .is_override = parameter.is_override,
                        .value_authored = parameter.value_authored,
                    });
                }
                object.prefab_instance = std::move(instance);
            }
            object.components.reserve(pending_object.components.size());
            for (std::size_t component_index = 0;
                 component_index < pending_object.components.size();
                 ++component_index) {
                const auto &pending_component = pending_object.components[component_index];
                ResolvedComponentOrigin origin = pending_component.generated
                    ? ResolvedComponentOrigin{*pending_component.generated}
                    : ResolvedComponentOrigin{AuthoredComponentOrigin{
                          *pending_component.authored_index}};
                object.components.push_back(resolveComponent(
                    pending_component.json, pending_component.codec,
                    pending_component.authored_index.value_or(0), component_index,
                    std::move(origin),
                    SceneComponentProvenance{
                        .scene_id = source_scene.scene_id,
                        .object_index = source_object.declaration_index,
                        .component_index = component_index,
                    },
                    result.behavior_reload_sources_, scene_camera_defaults));
                if (!first_camera_resolved &&
                    object.components.back().name == "camera") {
                    scene_camera_defaults = cameraDefaultsAfter(
                        object.components.back(), scene_camera_defaults);
                    first_camera_resolved = true;
                }
            }
            scene.objects.push_back(std::move(object));
        }
        result.scenes_.push_back(std::move(scene));
    }

    result.prefab_closure_.generation = generation.value;
    result.prefab_closure_.registry_generation =
        result.prefab_registry_.generation();
    result.prefab_closure_.provider_generation =
        result.bindable_provider_.generation();
    result.prefab_closure_.provider_fingerprint_sha256 =
        std::string{result.bindable_provider_.fingerprint()};
    result.prefab_closure_.resolver_inputs_fingerprint_sha256 =
        resolverInputsFingerprint(defaults);

    result.prefab_closure_.scenes.reserve(authoring_scenes.size());
    for (const auto &scene : authoring_scenes) {
        result.prefab_closure_.scenes.push_back(
            {.scene_id = scene.scene_id,
             .digest_sha256 = sha256(scene.authoredJson().dump())});
    }
    std::sort(result.prefab_closure_.scenes.begin(),
              result.prefab_closure_.scenes.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.scene_id < rhs.scene_id;
              });

    for (const auto &scene : pending_scenes) {
        for (const auto &object : scene.objects) {
            if (object.prefab_record == nullptr) continue;
            result.prefab_closure_.prefabs.push_back(
                {.name = object.prefab_record->name,
                 .digest_sha256 = object.prefab_record->digest_sha256});
        }
    }
    std::sort(result.prefab_closure_.prefabs.begin(),
              result.prefab_closure_.prefabs.end(),
              [](const auto &lhs, const auto &rhs) {
                  return lhs.name < rhs.name;
              });
    result.prefab_closure_.prefabs.erase(
        std::unique(result.prefab_closure_.prefabs.begin(),
                    result.prefab_closure_.prefabs.end(),
                    [](const auto &lhs, const auto &rhs) {
                        return lhs.name == rhs.name;
                    }),
        result.prefab_closure_.prefabs.end());

    std::vector<std::string> behavior_names;
    behavior_names.reserve(result.behavior_reload_sources_.size());
    // Reload sources intentionally retain authored behaviors so missing DLLs
    // can remain pending and recover. The prefab closure only owns generated
    // behavior dependencies.
    for (const auto &scene : result.scenes_) {
        for (const auto &object : scene.objects) {
            for (const auto &component : object.components) {
                if (component.name != "behavior" ||
                    !component.origin.isGenerated()) {
                    continue;
                }
                behavior_names.push_back(
                    component.source_json_exact.value("type", std::string{}));
            }
        }
    }
    std::sort(behavior_names.begin(), behavior_names.end());
    behavior_names.erase(
        std::unique(behavior_names.begin(), behavior_names.end()),
        behavior_names.end());
    const auto dependencies = result.bindable_provider_.behaviorDependencies();
    // Legacy V1 callers historically resolve authored behaviors without an
    // explicit provider snapshot. Preserve that non-persistence path; every
    // production SnapshotV2/prefab route supplies a non-zero pinned snapshot.
    if (result.bindable_provider_.generation() == 0) behavior_names.clear();
    for (const auto &name : behavior_names) {
        const auto found = std::lower_bound(
            dependencies.begin(), dependencies.end(), name,
            [](const auto &dependency, std::string_view stable_name) {
                return dependency.stable_name < stable_name;
            });
        if (found == dependencies.end() || found->stable_name != name) {
            throw PrefabError{
                PrefabErrorCode::PrefabProviderStale,
                "behavior dependency is absent from provider snapshot: " + name,
                {.json_pointer = "/prefab_closure/behaviors"}};
        }
        result.prefab_closure_.behaviors.push_back(
            {.stable_name = found->stable_name,
             .schema_version = found->schema_version,
             .params_schema_fingerprint = found->params_schema_fingerprint});
    }
    return result;
}

SceneProjectionState ResolvedSceneResolver::prepare(
    AuthoringSceneDocument document, SceneResolverGeneration generation,
    ResolvedSceneDefaults defaults, PrefabRegistrySnapshot registry,
    BindableProviderSnapshot provider) {
    auto resolved = resolve(document, generation, defaults, std::move(registry),
                            std::move(provider));
    return SceneProjectionState{std::move(document), std::move(resolved)};
}

[[noreturn]] void ResolvedSceneResolver::resolveGeneratedComponent() {
    throw ResolvedSceneError{
        ResolvedSceneErrorCode::generated_component_unsupported,
        "WP360 preserves authored component identity only"};
}

} // namespace Pelican
