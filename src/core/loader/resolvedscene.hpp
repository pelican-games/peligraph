#pragma once

#include "authoringscenedocument.hpp"
#include "componentcodec.hpp"

#include <any>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

PELICAN_DEFINE_HANDLE(SceneResolverGeneration, std::uint64_t)

enum class ResolvedSceneErrorCode : std::uint8_t {
    generated_component_unsupported,
};

std::string_view resolvedSceneErrorCodeName(
    ResolvedSceneErrorCode code) noexcept;

class ResolvedSceneError : public std::runtime_error {
    ResolvedSceneErrorCode code_;

  public:
    ResolvedSceneError(ResolvedSceneErrorCode code, const std::string &message);
    ResolvedSceneErrorCode code() const noexcept { return code_; }
};

struct SceneComponentProvenance {
    std::string scene_id;
    std::size_t object_index = 0;
    std::size_t component_index = 0;
};

// Core-private reload input: source params are intentionally retained before
// active/candidate defaults are applied. Candidate validation canonicalizes
// this value exactly once.
struct BehaviorReloadSourceParams {
    std::string stable_name;
    nlohmann::json source_params;
    SceneComponentProvenance provenance;
};

// Every authored omission which affects a scene reader is completed while the
// immutable projection is prepared.  Keeping the project camera defaults on
// the projection also lets editor transactions and request-local previews
// reproduce the same completion without consulting live modules.
struct ResolvedSceneDefaults {
    CameraProjectionSpec camera_projection;
    CameraSpritePolicySpec camera_sprite;
};

struct ResolvedComponent {
    std::size_t authoring_component_index = 0;
    std::string name;
    nlohmann::json source_json_exact;
    nlohmann::json effective_json;
    ComponentCodecQueryMetadata codec;
    const ComponentCodec *runtime_codec = nullptr;
    ComponentCodecValue runtime_value;
    // Authoring accepts inactive-scene runtime errors and historically raises
    // them only when that scene is bound. Resolve the error once, retain it in
    // the projection, and let binders rethrow it without decoding raw JSON.
    std::optional<std::string> runtime_resolution_error;
    std::optional<std::string> behavior_canonical_params;
    bool behavior_pending = false;

    const ComponentCodecValue &requireRuntimeValue() const;
};

struct ResolvedObject {
    AuthoringObjectId authoring_object_id{};
    std::size_t authoring_object_index = 0;
    std::optional<std::string> name;
    std::optional<std::string> parent;
    std::vector<ResolvedComponent> components;
};

struct ResolvedSceneView {
    std::string scene_id;
    std::vector<ResolvedObject> objects;
};

class ResolvedScene {
    SceneRevision revision_{};
    SceneResolverGeneration resolver_generation_{};
    std::vector<std::string> warnings_;
    std::vector<ResolvedSceneView> scenes_;
    std::vector<BehaviorReloadSourceParams> behavior_reload_sources_;
    ResolvedSceneDefaults defaults_;

    friend class ResolvedSceneResolver;

  public:
    SceneRevision revision() const noexcept { return revision_; }
    SceneResolverGeneration resolverGeneration() const noexcept {
        return resolver_generation_;
    }
    std::span<const std::string> warnings() const noexcept { return warnings_; }
    std::span<const ResolvedSceneView> scenes() const noexcept { return scenes_; }
    std::span<const BehaviorReloadSourceParams> behaviorReloadSources() const
        noexcept {
        return behavior_reload_sources_;
    }
    const ResolvedSceneDefaults &defaults() const noexcept { return defaults_; }

    const ResolvedSceneView *findScene(std::string_view scene_id) const noexcept;
};

// Atomic ownership unit. The authoring and resolved halves are never
// published independently, and their revisions are checked at construction.
class SceneProjectionState {
    AuthoringSceneDocument authoring_;
    ResolvedScene resolved_;

    SceneProjectionState(AuthoringSceneDocument authoring,
                         ResolvedScene resolved);
    friend class ResolvedSceneResolver;
    friend class ProjectBasicConfig;
    friend class ProjectBasicConfigProjectionTarget;

  public:
    const AuthoringSceneDocument &authoring() const noexcept {
        return authoring_;
    }
    const ResolvedScene &resolved() const noexcept { return resolved_; }
    SceneRevision revision() const noexcept { return authoring_.revision(); }
    SceneResolverGeneration resolverGeneration() const noexcept {
        return resolved_.resolverGeneration();
    }
    void swap(SceneProjectionState &other) noexcept;
};

class ResolvedSceneResolver {
  public:
    static ResolvedScene resolve(const AuthoringSceneDocument &document,
                                 SceneResolverGeneration generation,
                                 ResolvedSceneDefaults defaults = {});
    static SceneProjectionState prepare(AuthoringSceneDocument document,
                                        SceneResolverGeneration generation,
                                        ResolvedSceneDefaults defaults = {});

    [[noreturn]] static void resolveGeneratedComponent();
};

} // namespace Pelican
