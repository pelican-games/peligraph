#pragma once

#include "../loader/pathresolver.hpp"
#include "skeletalanimation.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <variant>

namespace Pelican {

// The clip component's asset contract, shared by edit admission and ECS.
// Graph components have a different binding path and do not call this.
inline const SkeletalAnimationClip &requireAnimationComponentClip(
    const SkeletalModelData *model, const std::string &clip, const PathResolver &resolver) {
    if (!model) {
        throw std::runtime_error("animation component clip '" + clip +
                                 "' is attached to a model without a glTF skin");
    }
    const auto resolved = resolver.resolveExistingFileReference(clip);
    const auto *reference = std::get_if<ResolvedPathFragment>(&resolved);
    if (reference == nullptr || reference->fragment.kind != "animation") {
        throw std::runtime_error("animation component clip must be a #animation fragment: " + clip);
    }
    std::error_code source_error, clip_error;
    const auto model_source = std::filesystem::weakly_canonical(model->source_path, source_error);
    const auto clip_source = std::filesystem::weakly_canonical(reference->path, clip_error);
    if (source_error || clip_error || model_source != clip_source) {
        throw std::runtime_error("animation component clip source does not match the skinned model: " + clip);
    }
    return findAnimationClip(*model, reference->fragment.path);
}

} // namespace Pelican
