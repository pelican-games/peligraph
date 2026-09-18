#pragma once

#include "animationjobs.hpp"
#include "../model/modeltemplate.hpp"
#include "../renderer/modelinstance.hpp"
#include "../userpublic/details/ecs/entity.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace Pelican::Animation::Internal {

// D-7. The one record shared by all three parts of the service. `SinkRecord`
// and `StagedFrame` keep a raw pointer to it, and the object table itself
// stays on `AnimationServiceRuntime::Impl` -- the parts only ever compare the
// pointer or read through it, they never own one.
struct ObjectRecord {
    std::string name;
    const SkeletalModelData *model{};
    const AnimationAsset *asset{};
    ModelAssetId logical_asset{};
    std::optional<ModelInstanceId> renderer_instance;
    std::optional<GameObjectId> scene_entity;
    std::uint64_t scene_epoch{};
};

} // namespace Pelican::Animation::Internal
