#include "../userpublic/userdataerror.hpp"
#include "graphinputs.hpp"

#include "../../project/graphmodule.hpp"
#include "../loader/scene.hpp"
#include "../ecs/core.hpp"
#include "../ecs/predefined/modelview.hpp"
#include "../userpublic/components/predefined.hpp"
#include "../renderer/polygoninstancecontainer.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif

#include <stdexcept>

namespace Pelican {
namespace {
// AT-1 decision 3: the audio clock, the second of the sound heard now -- as
// the frame's head read it, so every evaluation of the frame reads the same
// number. Without an audio clock of its own (the null backend, a build or a
// host without audio) the game clock stands in for it.
double frameAudioTime(double time) {
#if PELICAN_WITH_AUDIO
    if (const auto *audio = FastModuleContainer::tryGet<Audio>()) return audio->clock(time).audio_time;
#endif
    return time;
}
} // namespace

void validateGraphInputs(const ValueGraph::Graph &graph) {
    auto names = graph.inputNames();
    // CT-1: the table the catalog lists under value.input (input_names).
    for (const auto &input : graphFrameInputNames()) names.erase(std::string{input.name});
    for (const auto &[node, source] : graph.morphSources()) names.erase("$morph/" + node);
    if (!names.empty()) throw std::runtime_error("unbound frame graph input: " + *names.begin());
}

ValueGraph::Inputs collectGraphInputs(const ValueGraph::Graph &graph, const SceneLoader &scene,
    const ValueGraph::InactiveScopes &inactive, double time, double delta_time) {
    ValueGraph::Inputs inputs{{"time", time}, {"delta_time", delta_time},
        {"scene_time", time - scene.sceneStartTime()}, {"audio_time", frameAudioTime(time)}};
    for (const auto &[node, source] : graph.morphSources()) {
        if (!graph.nodeActive(node, inactive)) continue;
        const auto object = scene.graphOwnerObject(source.owner);
        if (!object) throw UserDataError("graph morph owner is unavailable: " + node);
        const auto *model = GET_MODULE(ECSCore).getTemplatePublicModule()
            .tryComponent<SimpleModelViewComponent>(*object);
        const auto weight = model && model->model_instance_id
            ? GET_MODULE(PolygonInstanceContainer).morphWeight(*model->model_instance_id, source.node, source.target)
            : std::nullopt;
        if (!weight) throw UserDataError("graph morph target is unavailable: " + node);
        inputs["$morph/" + node] = *weight;
    }
    return inputs;
}
}
