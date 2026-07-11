#include "animationsystem.hpp"

#include "../../appflow/enginetime.hpp"
#include "../../asset/model.hpp"
#include "../../loader/pathresolver.hpp"
#include "../../model/skeletalanimation.hpp"
#include "../../renderer/polygoninstancecontainer.hpp"

#include <filesystem>
#include <stdexcept>
#include <variant>

namespace Pelican {

void AnimationSystem::process(QueryComponents components, size_t count) {
    auto animations = std::get<AnimationComponent *>(components);
    auto models = std::get<SimpleModelViewComponent *>(components);
    const auto time = GET_MODULE(EngineTime).now();
    for (size_t i = 0; i < count; ++i) {
        if (!models[i].model_instance_id) continue;
        auto &model = GET_MODULE(ModelAssetContainer).getModelTemplateByName(models[i].model_name);
        if (!model.skeletal) {
            throw std::runtime_error("animation component clip '" + animations[i].clip +
                                     "' is attached to a model without a glTF skin");
        }
        const auto resolved = GET_MODULE(PathResolver).resolveExistingFileReference(animations[i].clip);
        const auto *reference = std::get_if<ResolvedPathFragment>(&resolved);
        if (reference == nullptr || reference->fragment.kind != "animation") {
            throw std::runtime_error("animation component clip must be a #animation fragment: " +
                                     animations[i].clip);
        }
        std::error_code source_error, clip_error;
        const auto model_source = std::filesystem::weakly_canonical(model.skeletal->source_path, source_error);
        const auto clip_source = std::filesystem::weakly_canonical(reference->path, clip_error);
        if (source_error || clip_error || model_source != clip_source) {
            throw std::runtime_error("animation component clip source does not match the skinned model: " +
                                     animations[i].clip);
        }
        const auto &clip = findAnimationClip(*model.skeletal, reference->fragment.path);
        const auto palette = evaluateSkinPalette(*model.skeletal, &clip, time, animations[i].speed,
                                                 animations[i].loop != 0, animations[i].start_time);
        GET_MODULE(PolygonInstanceContainer).setSkinningPalette(*models[i].model_instance_id, palette);
    }
}

} // namespace Pelican
