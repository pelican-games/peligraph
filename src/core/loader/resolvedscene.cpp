#include "resolvedscene.hpp"

#include "authoringsceneauthority.hpp"
#include "../userpublic/details/behavior/registerer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
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
    const AuthoringComponentView &source,
    const SceneComponentProvenance &provenance,
    std::vector<BehaviorReloadSourceParams> &behavior_sources,
    const ResolvedSceneDefaults &defaults) {
    const auto &authored = source.authoredJson();
    if (requestsGeneratedComponent(authored)) {
        ResolvedSceneResolver::resolveGeneratedComponent();
    }

    ResolvedComponent result{
        .authoring_component_index = source.declaration_index,
        .name = authored.at("name").get<std::string>(),
        .source_json_exact = authored,
        .effective_json = authored,
        .codec = source.codec,
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

} // namespace

ResolvedScene ResolvedSceneResolver::resolve(
    const AuthoringSceneDocument &document,
    SceneResolverGeneration generation, ResolvedSceneDefaults defaults) {
    if (generation.value == 0) {
        throw std::invalid_argument(
            "SceneResolverGeneration must be non-zero");
    }

    // Acquiring the raw view here is intentional and type-visible: this is one
    // of the two authorized targets. All runtime readers consume the result.
    const auto raw_view = AuthoringSceneAuthority::rawView(document);
    (void)raw_view;
    const auto authoring_scenes = AuthoringSceneAuthority::query(document);

    ResolvedScene result;
    result.revision_ = document.revision();
    result.resolver_generation_ = generation;
    result.defaults_ = defaults;
    result.warnings_.assign(document.warnings().begin(),
                            document.warnings().end());
    result.scenes_.reserve(authoring_scenes.size());

    for (const auto &source_scene : authoring_scenes) {
        ResolvedSceneView scene{.scene_id = source_scene.scene_id};
        auto scene_camera_defaults = defaults;
        bool first_camera_resolved = false;
        scene.objects.reserve(source_scene.objects.size());
        for (std::size_t object_index = 0;
             object_index < source_scene.objects.size(); ++object_index) {
            const auto &source_object = source_scene.objects[object_index];
            ResolvedObject object{
                .authoring_object_id = source_object.authoring_object_id,
                .authoring_object_index = source_object.declaration_index,
                .name = source_object.name,
                .parent = source_object.parent,
            };
            object.components.reserve(source_object.components.size());
            for (std::size_t component_index = 0;
                 component_index < source_object.components.size();
                 ++component_index) {
                object.components.push_back(resolveComponent(
                    source_object.components[component_index],
                    SceneComponentProvenance{
                        .scene_id = source_scene.scene_id,
                        .object_index = source_object.declaration_index,
                        .component_index = source_object.components[component_index]
                                               .declaration_index,
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
    ResolvedSceneDefaults defaults) {
    auto resolved = resolve(document, generation, defaults);
    return SceneProjectionState{std::move(document), std::move(resolved)};
}

[[noreturn]] void ResolvedSceneResolver::resolveGeneratedComponent() {
    throw ResolvedSceneError{
        ResolvedSceneErrorCode::generated_component_unsupported,
        "WP360 preserves authored component identity only"};
}

} // namespace Pelican
