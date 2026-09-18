#include "scene.hpp"
#include "../../project/graphcomposition.hpp"
#include "../appflow/enginetime.hpp"
#include "../ecs/core.hpp"
#include "../log.hpp"
#include "../userpublic/gamecontext.hpp"
#if PELICAN_WITH_AUDIO
#include "../audio/audio.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace Pelican {
namespace {
ValueGraph::ParameterValues parameterValues(const auto &parameters) {
    ValueGraph::ParameterValues values;
    for (const auto &[name, parameter] : parameters) values.emplace(name, parameter.value);
    return values;
}
}

bool SceneLoader::ownsGraphParameter(const ValueGraph::Graph &graph, GameObjectId object,
                                    std::string_view parameter) const {
    return ownsGraphOwner(graph.parameterOwner(parameter), object);
}

SceneLoader::GraphRuntime::~GraphRuntime() {
#if PELICAN_WITH_AUDIO
    if (auto *audio = FastModuleContainer::tryGet<Audio>())
        for (const auto &[node, sound] : sounds) audio->stopSound(sound);
#endif
}

std::optional<GameObjectId> SceneLoader::graphOwnerObject(const GraphInstanceOwner &owner) const {
    if (owner.scene != currentScene()) return std::nullopt;
    for (const auto &binding : runtime_object_bindings)
        if (binding.authoring_object_id.value == owner.object &&
            GET_MODULE(ECSCore).getTemplatePublicModule().isAlive(binding.object_id)) return binding.object_id;
    return std::nullopt;
}

bool SceneLoader::ownsGraphOwner(const GraphInstanceOwner *owner, GameObjectId object) const {
    return owner && graphOwnerObject(*owner) == object;
}

std::map<std::string, GameObjectId> SceneLoader::graphOwnerObjects(const ValueGraph::Graph &graph) const {
    std::map<std::string, GameObjectId> owners;
    for (const auto &[scope, owner] : graph.instanceOwners())
        if (const auto object = graphOwnerObject(owner)) owners.emplace(scope, *object);
    return owners;
}

void SceneLoader::reportUnresolvedGraphOwners(
    const ValueGraph::Graph &graph,
    const std::map<std::string, GameObjectId> &owners) {
    for (const auto &[scope, object] : owners) {
        (void)object;
        graph_owner_resolved_scopes.emplace(scope);
    }
    std::string names;
    for (const auto &[scope, owner] : graph.instanceOwners()) {
        (void)owner;
        if (graph_owner_resolved_scopes.contains(scope)) continue;
        if (!graph_owner_reported_scopes.emplace(scope).second) continue;
        if (!names.empty()) names += ", ";
        names += scope;
    }
    if (names.empty() || logger == nullptr) return;
    // Said at graph generation synchronization, not per frame.
    // `inactiveGraphScopes` is read on the drawing path, so naming it there
    // would spam the log with a state that is normal while an object is being
    // republished.  A scope that has never resolved in this scene is different:
    // the document points at an object the scene does not have, and it stays
    // that way until someone edits it.  Both memos are cleared by a scene load
    // and only then, so a removed object's republished graph stays quiet while
    // a freshly loaded scene still reports what it cannot bind.
    LOG_WARNING(logger,
                "graph instance owner does not match any object in this scene: {}",
                names);
}

SceneLoader::GraphRuntime &SceneLoader::synchronizeGraphRuntime(
    const std::shared_ptr<const ValueGraph::Graph> &graph) {
    std::erase_if(graph_runtime, [](const auto &entry) { return entry.first.expired(); });
    auto &runtime = graph_runtime.try_emplace(graph, *graph).first->second;
    auto owners = graphOwnerObjects(*graph);
    reportUnresolvedGraphOwners(*graph, owners);
    ValueGraph::InactiveScopes changed;
    for (const auto &[scope, object] : runtime.owners) {
        const auto next = owners.find(scope);
        if (next == owners.end() || next->second != object) changed.insert(scope);
    }
    graph->resetState(runtime.evaluation, changed);
    graph->resetState(runtime.pose_evaluation, changed);
#if PELICAN_WITH_AUDIO
    if (auto *audio = FastModuleContainer::tryGet<Audio>())
        std::erase_if(runtime.sounds, [&](const auto &entry) {
            if (!graph->nodeActive(entry.first, changed)) {
                audio->stopSound(entry.second);
                return true;
            }
            return !audio->soundState(entry.second);
        });
#endif
    runtime.owners = std::move(owners);
    auto &parameters = runtime.parameters;
    std::erase_if(parameters, [&](const auto &entry) {
        return !ownsGraphParameter(*graph, entry.second.object, entry.first);
    });
    return runtime;
}

bool SceneLoader::setGraphParameter(std::shared_ptr<const ValueGraph::Graph> graph, GameObjectId object,
                                   std::string_view parameter, std::optional<double> value) {
    if (value && !std::isfinite(*value)) throw std::invalid_argument("graph parameter must be finite");
    if (!ownsGraphParameter(*graph, object, parameter)) {
        // An edge supplies this port, so the expansion left no value.parameter
        // for an override to land on. Say so by name rather than reporting the
        // same `false` as an unknown parameter: the override is not applied and
        // must not disappear quietly.
        if (ownsGraphOwner(graph->shadowedParameterOwner(parameter), object))
            throw GraphRegistryError("graph.override_shadowed@1",
                {{"parameter", std::string{parameter}}});
        return false;
    }
    auto &parameters = synchronizeGraphRuntime(graph).parameters;
    if (value) parameters.insert_or_assign(std::string{parameter}, GraphParameterOverride{object, *value});
    else parameters.erase(std::string{parameter});
    return true;
}

ValueGraph::ParameterValues SceneLoader::graphParameterValues(std::shared_ptr<const ValueGraph::Graph> graph) {
    const auto &parameters = synchronizeGraphRuntime(graph).parameters;
    return parameterValues(parameters);
}

void SceneLoader::publishGraphValues(ValueGraph::PublishedInstanceInputs snapshot) {
    snapshot.published = true;
    graph_values = std::move(snapshot);
}

ValueGraph::EvaluationState &SceneLoader::graphEvaluationState(const std::shared_ptr<const ValueGraph::Graph> &graph) {
    return synchronizeGraphRuntime(graph).evaluation;
}

ValueGraph::EvaluationState &SceneLoader::poseGraphEvaluationState(
    const std::shared_ptr<const ValueGraph::Graph> &graph) {
    return synchronizeGraphRuntime(graph).pose_evaluation;
}

bool SceneLoader::emitGraphEvent(std::shared_ptr<const ValueGraph::Graph> graph, GameObjectId object,
                                std::string_view input, Animation::CurvePlaybackCommand command) {
    if (!ownsGraphOwner(graph->eventOwner(input), object)) return false;
    return graph->emitEvent(synchronizeGraphRuntime(graph).evaluation, input, command,
                            GET_MODULE(EngineTime).now() - scene_start_time);
}

bool SceneLoader::applyGraphAudioEvent(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                     std::string_view node, Animation::CurvePlaybackCommand command) {
#if PELICAN_WITH_AUDIO
    const auto source = graph->audioSources().find(std::string{node});
    if (source == graph->audioSources().end() || !graph->nodeActive(node, inactiveGraphScopes(*graph))) return false;
    auto &sounds = synchronizeGraphRuntime(graph).sounds;
    auto &audio = GET_MODULE(Audio);
    const auto existing = sounds.find(node);
    if (command == Animation::CurvePlaybackCommand::start) {
        const auto sound = audio.playSound(source->second);
        if (existing != sounds.end()) {
            audio.stopSound(existing->second);
            existing->second = sound;
        } else sounds.emplace(std::string{node}, sound);
    } else if (existing != sounds.end()) {
        if (command == Animation::CurvePlaybackCommand::reset) {
            audio.stopSound(existing->second);
            sounds.erase(existing);
        } else audio.setSoundPaused(existing->second, command == Animation::CurvePlaybackCommand::pause);
    }
    return true;
#else
    return false;
#endif
}

std::optional<SoundHandle> SceneLoader::graphSound(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                                std::string_view node) const {
    const auto runtime = graph_runtime.find(graph);
    if (runtime == graph_runtime.end()) return std::nullopt;
    const auto sound = runtime->second.sounds.find(node);
    return sound == runtime->second.sounds.end() ? std::nullopt : std::optional{sound->second};
}

void SceneLoader::updateGraphRuntime() {
    std::erase_if(graph_runtime, [](const auto &entry) { return entry.first.expired(); });
    std::vector<std::shared_ptr<const ValueGraph::Graph>> graphs;
    for (const auto &[weak, runtime] : graph_runtime)
        if (auto graph = weak.lock()) graphs.push_back(std::move(graph));
    for (const auto &graph : graphs) (void)synchronizeGraphRuntime(graph);
}

std::unique_ptr<SceneLoader::PreparedGraphRuntimeTransfer> SceneLoader::prepareGraphRuntimeTransfer(
    const std::shared_ptr<const ValueGraph::Graph> &previous,
    const std::shared_ptr<const ValueGraph::Graph> &next) {
    if (previous == next) return {};
    auto prepared = std::unique_ptr<PreparedGraphRuntimeTransfer>(new PreparedGraphRuntimeTransfer(*this, previous));
    if (!next) return prepared;
    auto &runtime = prepared->prepared_.try_emplace(next, *next).first->second;
    runtime.owners = graphOwnerObjects(*next);
    if (!previous || !graph_runtime.contains(previous)) return prepared;
    auto &old = synchronizeGraphRuntime(previous);
    auto transferred = next->transferState(*previous, old.evaluation);
    runtime.evaluation = std::move(transferred.state);
    // The animation partition reads the same document through its own state.
    // Both halves cross a generation change together; leaving one behind would
    // restart the pose side's clocks while the rendering side kept running.
    runtime.pose_evaluation = next->transferState(*previous, old.pose_evaluation).state;
    ValueGraph::InactiveScopes changed;
    for (const auto &[scope, object] : old.owners) {
        const auto current = runtime.owners.find(scope);
        if (current == runtime.owners.end() || current->second != object) changed.insert(scope);
    }
    next->resetState(runtime.evaluation, changed);
    next->resetState(runtime.pose_evaluation, changed);
    for (const auto &[name, parameter] : old.parameters)
        if (transferred.retained_parameters.contains(name) && ownsGraphParameter(*next, parameter.object, name))
            runtime.parameters.emplace(name, parameter);
    for (const auto &[node, sound] : old.sounds)
        if (transferred.retained_nodes.contains(node) && next->nodeActive(node, changed))
            prepared->retained_sounds_.emplace(node, sound);
    return prepared;
}

ValueGraph::ParameterValues SceneLoader::PreparedGraphRuntimeTransfer::parameters() const {
    return parameterValues(prepared_.begin()->second.parameters);
}

ValueGraph::EvaluationState &SceneLoader::PreparedGraphRuntimeTransfer::evaluation() {
    return prepared_.begin()->second.evaluation;
}

bool SceneLoader::PreparedGraphRuntimeTransfer::stageAudioEvent(
    std::string_view node, Animation::CurvePlaybackCommand command) {
    audio_events_.emplace_back(node, command);
    return true;
}

bool SceneLoader::PreparedGraphRuntimeTransfer::stageAudioValue(
    std::string_view node, bool voice, ValueGraph::AudioValueKind kind, float value) {
    if (!voice) return false;
    audio_values_.push_back({std::string{node}, kind, value});
    return true;
}

void SceneLoader::PreparedGraphRuntimeTransfer::publishAudio(
    const std::shared_ptr<const ValueGraph::Graph> &graph) {
    if (!committed_) throw std::logic_error("graph audio requires a published runtime");
    for (const auto &[node, command] : audio_events_)
        if (!scene_.applyGraphAudioEvent(graph, node, command))
            throw std::runtime_error("published graph audio event is unavailable: " + node);
    for (const auto &value : audio_values_)
        if (const auto sound = scene_.graphSound(graph, value.node)) {
            const GameContext context;
            const bool applied = value.kind == ValueGraph::AudioValueKind::volume
                ? context.audio().setSoundVolume(*sound, value.value) : context.audio().setSoundPitch(*sound, value.value);
            if (!applied) throw std::runtime_error("published graph voice is unavailable: " + value.node);
        }
    audio_events_.clear();
    audio_values_.clear();
}

void SceneLoader::PreparedGraphRuntimeTransfer::commit() noexcept {
    if (committed_) return;
    const auto old = scene_.graph_runtime.find(previous_);
    if (old != scene_.graph_runtime.end()) {
        for (const auto &[node, sound] : retained_sounds_) old->second.sounds.erase(node);
        if (!prepared_.empty()) prepared_.begin()->second.sounds.swap(retained_sounds_);
        scene_.graph_runtime.erase(old);
    }
    scene_.graph_runtime.merge(prepared_);
    committed_ = true;
}

ValueGraph::InactiveScopes SceneLoader::inactiveGraphScopes(const ValueGraph::Graph &graph) const {
    ValueGraph::InactiveScopes inactive;
    const auto owners = graphOwnerObjects(graph);
    for (const auto &[scope, owner] : graph.instanceOwners()) {
        if (!owners.contains(scope)) inactive.insert(scope);
    }
    return inactive;
}

} // namespace Pelican
