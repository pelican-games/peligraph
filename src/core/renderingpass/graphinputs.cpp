#include "../userpublic/userdataerror.hpp"
#include "graphinputs.hpp"

#include "../../project/graphmodule.hpp"
#include "../launchconfig.hpp"
#include "../loader/scene.hpp"
#include "../os/liveinputs.hpp"
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
bool offlineLaunchReadsNoLiveInputs() {
    const auto *launch = FastModuleContainer::tryGet<EngineLaunchConfig>();
    if (launch == nullptr) return false;
    return launch->start_time_explicit || launch->start_frames > 0 ||
           launch->camera_bake_output.has_value() || launch->input_replay ||
           launch->input_replay_path.has_value();
}

std::string liveName(std::string_view input) {
    return std::string{input.substr(std::string_view{"live:"}.size())};
}

// Validates the live:* names a graph reads, records that this project depends
// on one, and refuses deterministic offline output that would silently use a
// value a recording does not carry. Other names are left for the caller's old
// unbound-frame-input refusal.
void checkLiveInputReads(const ValueGraph::Graph &graph) {
    auto &live = GET_MODULE(LiveInputs);
    for (const auto &input : graph.inputNames()) {
        if (!input.starts_with("live:")) continue;
        if (!live.isDeclared(liveName(input)))
            throw std::runtime_error("live_input.unknown@1: graph reads undeclared live input '" +
                                     liveName(input) + "'");
        live.noteGraphReadsLive(input);
        if (offlineLaunchReadsNoLiveInputs())
            throw std::runtime_error(
                "offline.live_input_unrecorded@1: rendering graph reads " + input);
    }
}
} // namespace

void validateGraphInputs(const ValueGraph::Graph &graph) {
    auto names = graph.inputNames();
    // CT-1: the table the catalog lists under value.input (input_names).
    for (const auto &input : graphFrameInputNames()) names.erase(std::string{input.name});
    for (const auto &[node, source] : graph.morphSources()) names.erase("$morph/" + node);
    // OSC-1a: live:<name> is supplied from the frozen declaration, not from
    // the clock table. An undeclared name gets its own refusal so an author
    // can tell a typo from an unknown clock.
    for (auto current = names.begin(); current != names.end();) {
        if (!current->starts_with("live:")) {
            ++current;
            continue;
        }
        if (!GET_MODULE(LiveInputs).isDeclared(liveName(*current)))
            throw std::runtime_error("live_input.unknown@1: graph reads undeclared live input '" +
                                     liveName(*current) + "'");
        current = names.erase(current);
    }
    checkLiveInputReads(graph);
    if (!names.empty()) throw std::runtime_error("unbound frame graph input: " + *names.begin());
}

ValueGraph::Inputs collectGraphInputs(const ValueGraph::Graph &graph, const SceneLoader &scene,
    const ValueGraph::InactiveScopes &inactive, double time, double delta_time) {
    checkLiveInputReads(graph);
    ValueGraph::Inputs inputs{{"time", time}, {"delta_time", delta_time},
        {"scene_time", time - scene.sceneStartTime()}, {"audio_time", frameAudioTime(time)}};
    // OSC-1a: every evaluation of this frame reads the same frozen snapshot;
    // pending RPC writes are invisible until the next freeze.
    for (auto &[name, value] : GET_MODULE(LiveInputs).frozenInputs())
        inputs.emplace(std::move(name), value);
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
