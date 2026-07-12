#pragma once

#include "../userpublic/animation/abi_v1.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace Pelican {
struct SkeletalModelData;
namespace internal {
using RegistrationOwner = std::uint64_t;
}
namespace Animation {

// Engine-side owner of the additive A1.1 service. Game DLLs only see abi_v1.hpp.
class AnimationServiceRuntime {
  public:
    AnimationServiceRuntime();
    ~AnimationServiceRuntime();
    AnimationServiceRuntime(const AnimationServiceRuntime &) = delete;
    AnimationServiceRuntime &operator=(const AnimationServiceRuntime &) = delete;

    void registerObject(std::string name, const SkeletalModelData &model);
    void reset();
    void releaseOwner(internal::RegistrationOwner owner) noexcept;
    Status runPhases(AnimationSinkHandle sink, std::uint64_t frame_revision) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend AnimationServiceRuntime &animationServiceRuntime();
    friend Status getApiV1(std::uint32_t, ApiV1 *) noexcept;
};

AnimationServiceRuntime &animationServiceRuntime();
void releaseAnimationOwner(internal::RegistrationOwner owner) noexcept;

} // namespace Animation
} // namespace Pelican
