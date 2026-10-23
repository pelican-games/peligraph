#pragma once
#include "../userpublic/userdataerror.hpp"

#include "../loader/pathresolver.hpp"
#include "skeletalanimation.hpp"

#include <filesystem>
#if PELICAN_WITH_ANIMATION
#include "../userpublic/animation/animgraph.hpp"
#include "../userpublic/components/animation.hpp"
#include <algorithm>
#include <cmath>
#endif
#include <stdexcept>
#include <string>
#include <variant>

namespace Pelican {

// The clip component's asset contract, shared by edit admission and ECS.
// Graph components have a different binding path and do not call this.
inline const SkeletalAnimationClip &requireAnimationComponentClip(
    const SkeletalModelData *model, const std::string &clip, const PathResolver &resolver) {
    if (!model) {
        throw UserDataError("animation component clip '" + clip +
                                 "' is attached to a model without a glTF skin");
    }
    const auto resolved = [&] {
        try { return resolver.resolveExistingFileReference(clip); }
        catch (const std::runtime_error &error) { throw UserDataError(error.what()); }
    }();
    const auto *reference = std::get_if<ResolvedPathFragment>(&resolved);
    if (reference == nullptr || reference->fragment.kind != "animation") {
        throw UserDataError("animation component clip must be a #animation fragment: " + clip);
    }
    std::error_code source_error, clip_error;
    const auto model_source = std::filesystem::weakly_canonical(model->source_path, source_error);
    const auto clip_source = std::filesystem::weakly_canonical(reference->path, clip_error);
    if (source_error || clip_error || model_source != clip_source) {
        throw UserDataError("animation component clip source does not match the skinned model: " + clip);
    }
    try { return findAnimationClip(*model, reference->fragment); }
    catch (const std::runtime_error &error) { throw UserDataError(error.what()); }
}

#if PELICAN_WITH_ANIMATION
inline AnimationGraph::DocumentV1 parseAnimationComponentGraph(std::string_view text) {
    try { return AnimationGraph::parseDocumentV1(text); }
    catch (const std::runtime_error &error) { throw UserDataError(error.what()); }
    catch (const std::invalid_argument &error) { throw UserDataError(error.what()); }
    catch (const nlohmann::json::exception &error) { throw UserDataError(error.what()); }
}
inline AnimationGraph::DocumentV1 requireAnimationComponentGraph(
    const SkeletalModelData *model, const AnimationComponent &animation, const PathResolver &resolver) {
    if (!model) throw UserDataError("animation graph requires a skinned model");
    auto document = [&] {
        try { return parseAnimationComponentGraph(resolver.loadText(animation.graph)); }
        catch (const std::runtime_error &error) { throw UserDataError(error.what()); }
    }();
    for (const auto &state : document.states)
        for (const auto &clip : state.clips) {
            try { (void)findAnimationClip(*model, clip.clip); }
            catch (const std::runtime_error &error) { throw UserDataError(error.what()); }
        }
    if (animation.parameters)
        for (const auto &row : animation.parameters->rows)
            if (!std::isfinite(row.value.value) || std::none_of(document.parameters.begin(),
                document.parameters.end(), [&](const auto &parameter) { return parameter.name == row.value.parameter; }))
                throw UserDataError("animation graph parameter override failed: " + row.value.parameter);
    return document;
}
#endif

} // namespace Pelican
