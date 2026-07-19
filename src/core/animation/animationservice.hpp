#pragma once

#include "../renderer/modelinstance.hpp"
#include "../userpublic/animation/abi_v1.hpp"
#include "../userpublic/animation/pose_staging_v1.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Pelican {
struct SkeletalModelData;
struct VrmaRetargetedClip;
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
    void registerObject(std::string name, const SkeletalModelData &model,
                        ModelInstanceId renderer_instance);
    // Registers a retargeted .vrma asset in the same named Clip/Cursor
    // vocabulary used by anim_graph. Reload preserves logical identity and
    // advances only this source asset's generation.
    Status registerVrmaSource(
        std::string object_name, std::string source_name,
        std::shared_ptr<const VrmaRetargetedClip> clip) noexcept;
    Status reloadVrmaSource(
        std::string_view object_name, std::string_view source_name,
        std::shared_ptr<const VrmaRetargetedClip> replacement) noexcept;
    // Frame-boundary model replacement. Only objects bound to previous are
    // advanced to the replacement generation; unrelated objects keep running.
    void reloadAsset(const SkeletalModelData *previous,
                     const SkeletalModelData *replacement);
    void reset();
    void releaseOwner(internal::RegistrationOwner owner) noexcept;
    Status runPhases(AnimationSinkHandle sink, std::uint64_t frame_revision) noexcept;
    Status runAllPhases(std::uint64_t frame_revision) noexcept;
    std::uint64_t registrationGeneration() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend AnimationServiceRuntime &animationServiceRuntime();
    friend Status getApiV1(std::uint32_t, ApiV1 *) noexcept;
    friend Status getPoseStagingServiceV1(
        std::uint32_t, PoseStagingServiceV1 *) noexcept;
};

AnimationServiceRuntime &animationServiceRuntime();
void releaseAnimationOwner(internal::RegistrationOwner owner) noexcept;

} // namespace Animation
} // namespace Pelican
