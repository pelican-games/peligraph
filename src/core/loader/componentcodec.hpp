#pragma once

#include "../cameradefinition.hpp"
#include "../userpublic/details/schema/structfieldschema.hpp"
#include "resolutionresult.hpp"

#include <any>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican {

struct TransformComponent;
struct LocalTransformComponent;

enum class ComponentCodecRuntimeKind : std::uint8_t {
    Ecs,
    Camera,
    Light,
    Collider,
};

// The editor projection route is part of codec registration.  There is no
// fallback value: adding a codec requires choosing the runtime participant
// which owns its live projection.
enum class EditorProjectionPolicy : std::uint8_t {
    TransformClosure,
    RendererModel,
    EcsCodec,
    Camera,
    Light,
    Collider,
};

enum class EditorCodecSwapPhase : std::uint8_t {
    Publish,
    Rollback,
    Finish,
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
    bool rotation_specified = false;
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

enum class CameraControllerCodecType : std::uint8_t {
    Orbit,
    Follow,
    Fly,
};

struct CameraControllerCodecData {
    CameraControllerCodecType type = CameraControllerCodecType::Orbit;
    std::string target;
    vec3 offset{0.0f, 0.0f, 0.0f};
    float distance = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float damping = 0.0f;
    float speed = 0.0f;
    float sensitivity = 0.0f;
};

struct CameraCodecData {
    bool projection_specified = false;
    CameraProjectionKind projection_kind = CameraProjectionKind::Perspective;
    std::optional<float> yfov;
    std::optional<float> znear;
    std::optional<float> zfar;
    std::optional<float> aspect;
    std::optional<float> xmag;
    std::optional<float> ymag;

    bool sprite_specified = false;
    CameraPixelPerfectMode pixel_perfect = CameraPixelPerfectMode::off;
    CameraSpriteSortPolicy sprite_sort = CameraSpriteSortPolicy::z;
    std::optional<CameraControllerCodecData> controller;
};

// Shared complete-camera boundary used by editor transaction and preview
// validation. The implementation delegates to the single pure cross-field
// rule.
std::optional<UserDataViolation> completedCameraCrossFieldViolation(
    const CameraCodecData &camera,
    std::span<const std::string> path_prefix = {});

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

// Canonical omission defaults consumed by both the decoder and the prefab
// provider.  Keeping the value behind this boundary lets tests prove that a
// codec-default change reaches the resolved component, instead of merely
// mutating a detached provider descriptor.
struct ComponentCodecCanonicalDefaults {
    float collider_radius = 0.5f;

    friend bool operator==(const ComponentCodecCanonicalDefaults &,
                           const ComponentCodecCanonicalDefaults &) = default;
};

ComponentCodecCanonicalDefaults componentCodecCanonicalDefaults() noexcept;

namespace testing {

class ScopedComponentCodecCanonicalDefaults final {
    ComponentCodecCanonicalDefaults previous_;

  public:
    explicit ScopedComponentCodecCanonicalDefaults(
        ComponentCodecCanonicalDefaults replacement);
    ~ScopedComponentCodecCanonicalDefaults() noexcept;

    ScopedComponentCodecCanonicalDefaults(
        const ScopedComponentCodecCanonicalDefaults &) = delete;
    ScopedComponentCodecCanonicalDefaults &operator=(
        const ScopedComponentCodecCanonicalDefaults &) = delete;
};

} // namespace testing

using ComponentCodecValue = std::any;

// Move-only owner for a codec-defined prepared runtime token.  The concrete
// token type never escapes the codec registration; generic editor adapters
// retain only this erased carrier and call the codec's editor_swap hook.
class ComponentEditorSwapToken {
    void *value_ = nullptr;
    void (*destroy_)(void *) noexcept = nullptr;

  public:
    ComponentEditorSwapToken() = default;

    template <class Token>
    static ComponentEditorSwapToken own(std::unique_ptr<Token> token) noexcept {
        ComponentEditorSwapToken result;
        result.value_ = token.release();
        result.destroy_ = [](void *value) noexcept {
            delete static_cast<Token *>(value);
        };
        return result;
    }

    ~ComponentEditorSwapToken() noexcept { reset(); }
    ComponentEditorSwapToken(ComponentEditorSwapToken &&other) noexcept
        : value_{std::exchange(other.value_, nullptr)},
          destroy_{std::exchange(other.destroy_, nullptr)} {}
    ComponentEditorSwapToken &operator=(
        ComponentEditorSwapToken &&other) noexcept {
        if (this == &other) return *this;
        reset();
        value_ = std::exchange(other.value_, nullptr);
        destroy_ = std::exchange(other.destroy_, nullptr);
        return *this;
    }
    ComponentEditorSwapToken(const ComponentEditorSwapToken &) = delete;
    ComponentEditorSwapToken &operator=(const ComponentEditorSwapToken &) = delete;

    explicit operator bool() const noexcept { return value_ != nullptr; }
    void reset() noexcept {
        if (value_ != nullptr) destroy_(value_);
        value_ = nullptr;
        destroy_ = nullptr;
    }

    template <class Token> Token &get() noexcept {
        return *static_cast<Token *>(value_);
    }
};

// Every registered codec owns exactly these five operations. runtime_apply and
// runtime_project use the target documented by runtime_kind:
//   transform -> TransformCodecTarget, other ECS codecs -> their component,
//   camera/light -> CameraCodecData/LightCodecData, collider -> ColliderComponent.
enum class ComponentDecodeSurface : std::uint8_t { runtime, preview, web };
std::optional<UserDataViolation> componentDecodeSurfaceViolation(
    std::string_view name, const nlohmann::json &authored, ComponentDecodeSurface surface);

struct ComponentCodec {
    using DecodeAuthored = ResolutionResult<ComponentCodecValue> (*)(
        const nlohmann::json &);
    using EncodeCanonical = nlohmann::ordered_json (*)(const ComponentCodecValue &);
    using Schema = const SchemaNode &(*)();
    using RuntimeApply = void (*)(const ComponentCodecValue &, void *);
    using RuntimeProject = ComponentCodecValue (*)(const void *);
    using EditorPrepare = ComponentEditorSwapToken (*)(
        void *ecs, std::uint32_t entity_index,
        std::uint32_t entity_generation, const ComponentCodecValue &);
    using EditorSwap = void (*)(ComponentEditorSwapToken &,
                                EditorCodecSwapPhase) noexcept;

    std::string_view name;
    ComponentCodecRuntimeKind runtime_kind;
    EditorProjectionPolicy editor_policy;
    DecodeAuthored decode_authored;
    EncodeCanonical encode_canonical;
    Schema schema;
    RuntimeApply runtime_apply;
    RuntimeProject runtime_project;
    EditorPrepare editor_prepare;
    EditorSwap editor_swap;

    ResolutionResult<ComponentCodecValue> decodeAuthored(
        const nlohmann::json &authored,
        ComponentDecodeSurface surface = ComponentDecodeSurface::runtime) const;
    nlohmann::ordered_json encodeCanonical(const ComponentCodecValue &value) const;
    const SchemaNode &schemaNode() const;
    std::vector<StructFieldSchema> fieldSchema() const;
    void applyRuntime(const ComponentCodecValue &value, void *target) const;
    ComponentCodecValue projectRuntime(const void *source) const;
    ComponentEditorSwapToken prepareEditorSwap(
        void *ecs, std::uint32_t entity_index,
        std::uint32_t entity_generation,
        const ComponentCodecValue &value) const;
    void swapEditorToken(ComponentEditorSwapToken &token,
                         EditorCodecSwapPhase phase) const noexcept;

    friend bool operator==(const ComponentCodec &left,
                           const ComponentCodec &right) noexcept {
        return left.name == right.name &&
               left.runtime_kind == right.runtime_kind &&
               left.editor_policy == right.editor_policy &&
               left.decode_authored == right.decode_authored &&
               left.encode_canonical == right.encode_canonical &&
               left.schema == right.schema &&
               left.runtime_apply == right.runtime_apply &&
               left.runtime_project == right.runtime_project &&
               left.editor_prepare == right.editor_prepare &&
               left.editor_swap == right.editor_swap;
    }
};

std::span<const ComponentCodec> componentCodecs();
const ComponentCodec *findComponentCodec(std::string_view name) noexcept;
const ComponentCodec &requireComponentCodec(std::string_view name);
ComponentCodecQueryMetadata componentCodecQueryMetadata(std::string_view name) noexcept;

namespace testing {

const ComponentCodec *findComponentCodecIncludingDisabled(
    std::string_view name) noexcept;

// Test registration uses the same registry read by resolution and production
// context capture.  The planner itself receives a copied catalog and never
// consults this scoped ambient value.
class ScopedComponentCodecCatalog final {
    std::vector<ComponentCodec> replacement_;
    const std::vector<ComponentCodec> *previous_ = nullptr;

  public:
    explicit ScopedComponentCodecCatalog(
        std::vector<ComponentCodec> replacement);
    ~ScopedComponentCodecCatalog() noexcept;

    ScopedComponentCodecCatalog(const ScopedComponentCodecCatalog &) = delete;
    ScopedComponentCodecCatalog &operator=(
        const ScopedComponentCodecCatalog &) = delete;
};

} // namespace testing

// Query representation mandated for transform: authored local TRS and the
// display-only world projection are never conflated.
nlohmann::ordered_json projectTransformRuntimeJson(const TransformCodecTarget &target);

} // namespace Pelican
