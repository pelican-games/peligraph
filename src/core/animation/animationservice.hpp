#pragma once

#include "../renderer/modelinstance.hpp"
#include "../userpublic/animation/abi_v1.hpp"
#include "../userpublic/animation/pose_staging_v1.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Pelican {
struct ModelAssetId;
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
                        ModelAssetId logical_asset);
    void registerObject(std::string name, const SkeletalModelData &model,
                        ModelInstanceId renderer_instance);
    // Retires only this object's sinks/sources. Shared animation assets and
    // other registered objects remain valid.
    bool unregisterObject(std::string_view name);
    // Owner-thread frame boundary: forget lazy scene registrations whose
    // entity, scene or renderer instance has changed. Resolution is lazy again.
    void synchronizeSceneObjects();
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
    void reloadAsset(ModelAssetId logical_asset,
                     const SkeletalModelData *previous,
                     const SkeletalModelData *replacement);
    void reset();
    void releaseOwner(internal::RegistrationOwner owner) noexcept;
    // PM-1a. Which phase callbacks a run calls. A frame that is not a game
    // step (paused) runs only the phases the engine registered: the ones a
    // game DLL registered belong to game code, which does not run then.
    enum class PhaseOwners {
        all,
        engine_only,
    };
    Status runPhases(AnimationSinkHandle sink, std::uint64_t frame_revision) noexcept;
    Status runAllPhases(std::uint64_t frame_revision) noexcept;
    Status runAllPhases(std::uint64_t frame_revision, PhaseOwners owners) noexcept;
    std::uint64_t registrationGeneration() const noexcept;

    // C-4. How large the four handle tables a game-DLL reload churns have
    // grown. Only `sources` is compacted on releaseOwner; the other three keep
    // tombstones because invalid_handle and stale_generation are different
    // answers there. A test that reloads in a loop watches `sources` stop
    // growing and the rest grow by a bounded amount.
    struct HandleTableSizes {
        std::size_t sources{};
        std::size_t phases{};
        std::size_t cursors{};
        std::size_t owners{};
    };
    HandleTableSizes handleTableSizesForTesting() const noexcept;

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
