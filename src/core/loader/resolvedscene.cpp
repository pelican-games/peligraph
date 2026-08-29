#include "resolvedscene.hpp"

#include "authoringsceneauthority.hpp"
#include "../userpublic/details/behavior/registerer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
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

bool requestsGeneratedComponent(const nlohmann::json &component) {
    const auto generated = component.find("generated");
    if (generated != component.end() && generated->is_boolean() &&
        generated->get<bool>()) {
        return true;
    }
    const auto origin = component.find("origin");
    return origin != component.end() && origin->is_string() &&
           origin->get_ref<const std::string &>() == "generated";
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

const PrefabParameterDeclaration &parameterDeclaration(
    const PrefabDocument &document, std::string_view name) {
    const auto found = std::find_if(document.parameters.begin(),
                                    document.parameters.end(),
                                    [&](const auto &parameter) {
                                        return parameter.name == name;
                                    });
    if (found == document.parameters.end()) {
        throw std::logic_error("validated prefab binding lost its declaration");
    }
    return *found;
}

std::string bindingProviderName(const PrefabComponentDocument &component) {
    if (component.name != "behavior") return component.name;
    const auto type = component.body.find("type");
    if (type == component.body.end() || !type->is_string() ||
        type->get_ref<const std::string &>().empty()) {
        throw PrefabError{
            PrefabErrorCode::PrefabBindingNotBindable,
            "behavior type is a non-bindable fixed discriminant",
            {.json_pointer = "/type"}};
    }
    return "behavior:" + type->get<std::string>();
}

nlohmann::json validateAndSubstitute(
    const PrefabDocument &document,
    const PrefabComponentDocument &component,
    const PrefabInstanceDeclaration &instance,
    const BindableProviderSnapshot &provider,
    std::string_view scene_id) {
    const auto provider_name = bindingProviderName(component);
    for (const auto &binding : component.bindings) {
        if (binding.json_pointer == "/name" || binding.json_pointer == "/id" ||
            binding.json_pointer == "/shape" || binding.json_pointer == "/type" ||
            binding.json_pointer == "/controller/type" ||
            binding.json_pointer == "/params/controller/type") {
            throw PrefabError{
                PrefabErrorCode::PrefabBindingNotBindable,
                "prefab discriminants are not bindable",
                {.scene_id = std::string{scene_id},
                 .instance_id = instance.instance_id,
                 .prefab = instance.ref,
                 .parameter = binding.parameter,
                 .json_pointer = binding.json_pointer}};
        }
    }
    for (const auto &binding : component.bindings) {
        const auto &parameter = parameterDeclaration(document, binding.parameter);
        const auto *descriptor = provider.findActive(
            provider_name, binding.json_pointer, component.body);
        PrefabErrorContext context{
            .scene_id = std::string{scene_id},
            .instance_id = instance.instance_id,
            .prefab = instance.ref,
            .parameter = binding.parameter,
            .json_pointer = binding.json_pointer,
        };
        if (descriptor == nullptr &&
            !provider.containsPath(provider_name, binding.json_pointer)) {
            throw PrefabError{PrefabErrorCode::PrefabBindingNotBindable,
                              "prefab binding path is not bindable: " +
                                  binding.json_pointer,
                              std::move(context)};
        }
        if (descriptor == nullptr) {
            throw PrefabError{PrefabErrorCode::PrefabBindingInactive,
                              "prefab binding is inactive for the fixed discriminant",
                              std::move(context)};
        }
        if (!prefabParameterMatchesDescriptor(parameter, *descriptor)) {
            throw PrefabError{PrefabErrorCode::PrefabParameterType,
                              "prefab parameter type does not match binding path",
                              std::move(context)};
        }
    }
    return substitutePrefabComponent(component, instance.parameters);
}

} // namespace

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
        provider = buildProductionBindableProviderSnapshot();
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
                auto concrete = validateAndSubstitute(
                    *object.prefab_record->document, component, *object.prefab,
                    provider, scene.source->scene_id);
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
                    .closure_generation = result.prefab_registry_.generation(),
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
