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

// A value, or the named reason there is none.
template <class T> class Resolved {
  public:
    Resolved(T value) : value_{std::move(value)}, ok_{true} {}
    Resolved(ObjectError error) : error_{error}, ok_{false} {}

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // Null when there is no value.
    const T *get() const noexcept { return ok_ ? &value_ : nullptr; }
    // Throws std::logic_error whose message starts with the error's name when
    // there is no value.
    const T &value() const {
        if (!ok_) {
            throw std::logic_error(std::string{objectErrorName(error_)} +
                                   ": the result holds no value");
        }
        return value_;
    }

    // Meaningful only when !ok().
    ObjectError error() const noexcept { return error_; }
    // The error's name, or empty when there is a value.
    std::string_view errorName() const noexcept {
        return ok_ ? std::string_view{} : objectErrorName(error_);
    }

  private:
    T value_{};
    ObjectError error_ = ObjectError::not_materialized;
    bool ok_ = false;
};

// An object's transform in world space.
struct WorldTransform {
    vec3 pos{0.0f, 0.0f, 0.0f};
    quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    vec3 scale{1.0f, 1.0f, 1.0f};
};

} // namespace Pelican
