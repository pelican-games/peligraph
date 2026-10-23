#pragma once

#include "../src/core/animation/animationjobs.hpp"
#include "../src/core/ecs/predefined/animationsystem.hpp"

namespace Pelican::Animation {
struct AnimationAssetRegistryTestAccess {
    static const AnimationAsset &entry(const AnimationAssetRegistry &registry,
                                       const SkeletalModelData *key) {
        return registry.assets_.at(key);
    }
    // Force the stale-key collision that allocator address reuse would cause.
    // Keep the retired entry (including its expired identity) untouched.
    static void reuseKey(AnimationAssetRegistry &registry, const SkeletalModelData *old_key,
                         const SkeletalModelData *new_key) {
        auto entry = registry.assets_.extract(old_key);
        entry.key() = new_key;
        registry.assets_.insert(std::move(entry));
    }
};
}

namespace Pelican::test_support {
struct AnimationSystemTestAccess {
    static Animation::AnimationAssetRegistry &assets(AnimationSystem &system) {
        return system.assetRegistry();
    }
};
}
