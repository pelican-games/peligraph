#pragma once

#include "authoringscenedocument.hpp"
#include "componentcodec.hpp"
#include "prefabprovider.hpp"

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
#include <variant>
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

// SnapshotV2's resolver-input wire type is deliberately an alias instead of a
// second aggregate: its field set and declaration order are therefore exactly
// those of ResolvedSceneDefaults. Camera up and window dimensions are drawing
// inputs, not resolver inputs, and remain outside the snapshot contract.
using ResolverInputSnapshotV1 = ResolvedSceneDefaults;

// SnapshotV2's immutable dependency closure. Generation values are CAS tokens
// for an in-process publication; the remaining fields are the portable wire
// identity and deliberately exclude unused registry entries.
struct PrefabClosureScene {
    std::string scene_id;
    std::string digest_sha256;
    friend bool operator==(const PrefabClosureScene &,
                           const PrefabClosureScene &) = default;
};

struct PrefabClosureEntry {
    std::string name;
    std::string digest_sha256;
    friend bool operator==(const PrefabClosureEntry &,
                           const PrefabClosureEntry &) = default;
};

struct PrefabClosureBehavior {
    std::string stable_name;
    std::uint32_t schema_version = 0;
    std::string params_schema_fingerprint;
    friend bool operator==(const PrefabClosureBehavior &,
                           const PrefabClosureBehavior &) = default;
};

struct PrefabResolutionClosure {
    std::uint64_t generation = 0;
    std::uint64_t registry_generation = 0;
    std::uint64_t provider_generation = 0;
    std::vector<PrefabClosureScene> scenes;
    std::vector<PrefabClosureEntry> prefabs;
    std::vector<PrefabClosureBehavior> behaviors;
    std::string provider_fingerprint_sha256;
    std::string resolver_inputs_fingerprint_sha256;

    bool sameWireIdentity(const PrefabResolutionClosure &other) const noexcept;
};

// Canonical ResolverInputSnapshotV1 fingerprint. Its implementation mirrors
// every aggregate member with structured bindings, so adding an unencoded
// member is a compile-time failure.
std::string resolverInputsFingerprint(
    const ResolverInputSnapshotV1 &snapshot);

struct AuthoredComponentOrigin {
    std::size_t authoring_component_index = 0;
};

struct GeneratedComponentOrigin {
    std::string stable_generated_id;
    std::string prefab;
    std::string instance_id;
    std::string component_key;
    std::string digest_sha256;
};

enum class OriginTransitionDirection : std::uint8_t {
    GeneratedToAuthored,
    AuthoredToGenerated,
};

struct OriginTransitionEntry {
    std::string scene_id;
    AuthoringObjectId authoring_object_id{};
    std::string gid;
    std::string component_key;
    std::size_t final_authored_index = 0;

    friend bool operator==(const OriginTransitionEntry &,
                           const OriginTransitionEntry &) = default;
};

struct OriginTransitionMap {
    OriginTransitionDirection direction =
        OriginTransitionDirection::GeneratedToAuthored;
    std::vector<OriginTransitionEntry> entries;

    friend bool operator==(const OriginTransitionMap &,
                           const OriginTransitionMap &) = default;
};

struct OriginTransitionAuthority {
    std::string component_key;
    nlohmann::ordered_json body;
};

enum class OriginTransitionErrorCode : std::uint8_t {
    TargetMismatch,
    GeneratedIdSetMismatch,
    AuthoredIndexSetMismatch,
    AuthoredIndexInvalid,
    ComponentMismatch,
    ComponentKeyMismatch,
    GeneratedIdMismatch,
    AuthorityBodyMismatch,
};

std::string_view originTransitionErrorCodeName(
    OriginTransitionErrorCode code) noexcept;

class OriginTransitionError : public std::runtime_error {
    OriginTransitionErrorCode code_;

  public:
    OriginTransitionError(OriginTransitionErrorCode code,
                          std::string message);
    OriginTransitionErrorCode code() const noexcept { return code_; }
};

class ResolvedComponentOrigin {
    std::variant<AuthoredComponentOrigin, GeneratedComponentOrigin> value_;

    explicit ResolvedComponentOrigin(GeneratedComponentOrigin generated)
        : value_{std::move(generated)} {}
    friend class ResolvedSceneResolver;

  public:
    ResolvedComponentOrigin(AuthoredComponentOrigin authored = {})
        : value_{authored} {}
    bool isGenerated() const noexcept {
        return std::holds_alternative<GeneratedComponentOrigin>(value_);
    }
    const AuthoredComponentOrigin *authored() const noexcept {
        return std::get_if<AuthoredComponentOrigin>(&value_);
    }
    const GeneratedComponentOrigin *generated() const noexcept {
        return std::get_if<GeneratedComponentOrigin>(&value_);
    }
};

struct ResolvedComponent {
    std::size_t authoring_component_index = 0;
    std::size_t merged_component_index = 0;
    ResolvedComponentOrigin origin;
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

struct ResolvedPrefabParameter {
    std::string name;
    nlohmann::ordered_json value_resolved;
    bool is_override = false;
    std::optional<nlohmann::json> value_authored;
};

struct ResolvedPrefabInstance {
    std::string ref;
    std::string instance_id;
    std::uint64_t closure_generation = 0;
    std::uint64_t provider_generation = 0;
    std::vector<ResolvedPrefabParameter> parameters;
};

struct ResolvedObject {
    AuthoringObjectId authoring_object_id{};
    std::size_t authoring_object_index = 0;
    std::optional<std::string> name;
    std::optional<std::string> parent;
    std::optional<ResolvedPrefabInstance> prefab_instance;
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
    PrefabRegistrySnapshot prefab_registry_;
    BindableProviderSnapshot bindable_provider_;
    PrefabResolutionClosure prefab_closure_;

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
    const PrefabRegistrySnapshot &prefabRegistry() const noexcept {
        return prefab_registry_;
    }
    const BindableProviderSnapshot &bindableProvider() const noexcept {
        return bindable_provider_;
    }
    const PrefabResolutionClosure &prefabClosure() const noexcept {
        return prefab_closure_;
    }

    const ResolvedSceneView *findScene(std::string_view scene_id) const noexcept;
};

OriginTransitionMap buildOriginTransitionMap(
    const ResolvedScene &generated_scene,
    const AuthoringSceneDocument &authored_document,
    const ResolvedScene &authored_scene, std::string_view scene_id,
    AuthoringObjectId object_id, std::string_view instance_id,
    std::span<const OriginTransitionAuthority> authority);

void validateOriginTransitionMap(
    const OriginTransitionMap &map, const ResolvedScene &generated_scene,
    const AuthoringSceneDocument &authored_document,
    const ResolvedScene &authored_scene, std::string_view scene_id,
    AuthoringObjectId object_id, std::string_view instance_id,
    std::span<const OriginTransitionAuthority> authority);

OriginTransitionMap invertOriginTransitionMap(const OriginTransitionMap &map);
nlohmann::ordered_json originTransitionMapJson(const OriginTransitionMap &map);
OriginTransitionMap originTransitionMapFromJson(const nlohmann::json &value);

// Atomic ownership unit. The authoring and resolved halves are never
// published independently, and their revisions are checked at construction.
class SceneProjectionState {
    AuthoringSceneDocument authoring_;
    ResolvedScene resolved_;

    SceneProjectionState(AuthoringSceneDocument authoring,
                         ResolvedScene resolved);
    friend class ResolvedSceneResolver;
    friend class ProjectBasicConfig;
    friend class MetadataCommitToken;
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
                                 ResolvedSceneDefaults defaults = {},
                                 PrefabRegistrySnapshot registry = {},
                                 BindableProviderSnapshot provider = {});
    static SceneProjectionState prepare(AuthoringSceneDocument document,
                                        SceneResolverGeneration generation,
                                        ResolvedSceneDefaults defaults = {},
                                        PrefabRegistrySnapshot registry = {},
                                        BindableProviderSnapshot provider = {});

    [[noreturn]] static void resolveGeneratedComponent();
};

} // namespace Pelican
