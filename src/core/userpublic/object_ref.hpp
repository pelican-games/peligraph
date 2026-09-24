#pragma once

#include <details/ecs/entity.hpp>
#include <geom/quat.hpp>
#include <geom/vec.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace Pelican {

namespace internal {
struct ObjectRefAccess;
struct TransientObjectRefAccess;
} // namespace internal

// The run of the scene an ObjectRef was resolved in. Every scene load -- a
// reload of the same scene included -- starts a new run, and a ref from an
// earlier run is refused rather than read against whatever now carries the
// same object id. The value is only meaningful inside the process that issued
// it.
struct ObjectAuthority {
    std::uint64_t run_nonce = 0;
    std::uint64_t scene_epoch = 0;

    bool operator==(const ObjectAuthority &) const = default;
};

// A scene object, as the authored scene declares it. It carries no name: the
// name is looked up once, by ObjectsContext::resolve, and the ref is what the
// game keeps. A default-constructed ref names nothing and is refused as
// stale.
class ObjectRef {
  public:
    ObjectRef() = default;

    ObjectAuthority authority() const noexcept { return authority_; }
    // The object's id inside its scene run (the authoring object id).
    std::uint64_t objectId() const noexcept { return object_id_; }

    bool operator==(const ObjectRef &) const = default;

  private:
    friend struct internal::ObjectRefAccess;
    ObjectRef(ObjectAuthority authority, std::uint64_t object_id) noexcept
        : authority_{authority}, object_id_{object_id} {}

    ObjectAuthority authority_{};
    std::uint64_t object_id_ = 0;
};

// An object the game made at run time with GameContext::spawn. It is not in
// the authored scene and has no authoring object id: it is named by where it
// was made -- the scene run (authority), the frame boundary of that run that
// materializes it (1 is the first boundary after the run began) and its
// place among that boundary's spawns and destroys. The same game input makes
// the same boundary and index in every run, so only the authority differs
// between two runs of one scene.
//
// It is a different type from ObjectRef on purpose: an authored ref and a
// transient ref share the authority but never the domain, so passing one
// where the other is meant does not compile. A default-constructed ref names
// nothing and is refused as stale.
class TransientObjectRef {
  public:
    TransientObjectRef() = default;

    ObjectAuthority authority() const noexcept { return authority_; }
    std::uint64_t boundary() const noexcept { return boundary_; }
    std::uint32_t spawnIndex() const noexcept { return spawn_index_; }

    bool operator==(const TransientObjectRef &) const = default;

  private:
    friend struct internal::TransientObjectRefAccess;
    TransientObjectRef(ObjectAuthority authority, std::uint64_t boundary,
                       std::uint32_t spawn_index) noexcept
        : authority_{authority}, boundary_{boundary}, spawn_index_{spawn_index} {}

    ObjectAuthority authority_{};
    std::uint64_t boundary_ = 0;
    std::uint32_t spawn_index_ = 0;
};

// Why GameContext::spawn refused a request, by name. Nothing was queued.
//   <word>.module_unavailable@1   the prefab has a component this build left
//                                 out (a collider without physics), the name
//                                 a scene load gives the same component
//   prefab_* / behavior_type_unknown@1
//                                 the prefab or its parameters do not resolve,
//                                 under the names a scene load uses
//   object.spawn_outside_scene@1  no scene is loaded
//   object.spawn_component_unsupported@1
//                                 the prefab has a light or a camera, which a
//                                 spawned object cannot carry yet
//   object.name_reserved_by_authored@1
//                                 the requested name is an authored object's
//   object.name_reserved_by_engine@1
//                                 the requested name begins with `spawn:` or
//                                 `pelican://`, the forms the engine names an
//                                 object nobody named by
//   object.name_duplicate@1       another spawned object of this scene run has
//                                 the requested name
//   object.spawn_sequence_exhausted@1
//                                 the scene run's boundary, a boundary's spawn
//                                 count or a prefab's behavior count ran past
//                                 what an attachment identifier can hold
//   object.spawn_invalid@1        a component of the expanded prefab does not
//                                 decode
class SpawnError final : public std::runtime_error {
    std::string code_;

  public:
    SpawnError(std::string code, const std::string &detail)
        : std::runtime_error(code + ": " + detail), code_{std::move(code)} {}

    const std::string &code() const noexcept { return code_; }
};

// Why an object could not be resolved, materialized or read. When more than
// one applies, the first in this order is the one reported.
enum class ObjectError : std::uint32_t {
    // object.stale_authority@1: the ref was resolved in an earlier scene run
    // (the scene was loaded or reloaded since).
    stale_authority = 1,
    // object.not_materialized@1: the current scene run has no live object by
    // that name or for that ref (never declared, removed, or declared without
    // anything that makes it a runtime object, such as a light-only object).
    not_materialized = 2,
    // object.missing_capability@1: the object is live but lacks what the
    // operation reads (worldTransform on an object without a transform).
    missing_capability = 3,
};

constexpr std::string_view objectErrorName(ObjectError error) noexcept {
    switch (error) {
    case ObjectError::stale_authority: return "object.stale_authority@1";
    case ObjectError::not_materialized: return "object.not_materialized@1";
    case ObjectError::missing_capability: return "object.missing_capability@1";
    }
    return "object.unknown_error@1";
}

// The name Resolved reports for an error. Each error enum a facade answers
// with has one of these beside it (RenderError: render_error.hpp).
constexpr std::string_view errorCodeName(ObjectError error) noexcept {
    return objectErrorName(error);
}

// A value, or the named reason there is none. E is the facade's error enum;
// the objects facade answers with ObjectError, the render facade with
// RenderError.
template <class T, class E = ObjectError> class Resolved {
  public:
    Resolved(T value) : value_{std::move(value)}, ok_{true} {}
    Resolved(E error) : error_{error}, ok_{false} {}

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // Null when there is no value.
    const T *get() const noexcept { return ok_ ? &value_ : nullptr; }
    // Throws std::logic_error whose message starts with the error's name when
    // there is no value.
    const T &value() const {
        if (!ok_) {
            throw std::logic_error(std::string{errorCodeName(error_)} +
                                   ": the result holds no value");
        }
        return value_;
    }

    // Meaningful only when !ok().
    E error() const noexcept { return error_; }
    // The error's name, or empty when there is a value.
    std::string_view errorName() const noexcept {
        return ok_ ? std::string_view{} : errorCodeName(error_);
    }

  private:
    T value_{};
    E error_{};
    bool ok_ = false;
};

// An object's transform in world space.
struct WorldTransform {
    vec3 pos{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    vec3 scale{1.0f, 1.0f, 1.0f};
};

} // namespace Pelican
