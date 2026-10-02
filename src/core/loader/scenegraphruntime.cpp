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
#if PELICAN_WITH_AUDIO
// How far an integrated voice on its own loop point may stand from the graph's
// time before it is brought back (AU-1).
constexpr double nativeResyncSeconds = 0.25;
#endif
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
    std::erase_if(runtime.sound_epochs, [&](const auto &entry) { return !runtime.sounds.contains(entry.first); });
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

bool SceneLoader::applyGraphVoice(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                  std::string_view node, const ValueGraph::AudioVoiceDirective &directive) {
#if PELICAN_WITH_AUDIO
    using Phase = ValueGraph::AudioVoiceDirective::Phase;
    const auto source = graph->audioSources().find(std::string{node});
    if (source == graph->audioSources().end() || !graph->nodeActive(node, inactiveGraphScopes(*graph))) return false;
    auto &runtime = synchronizeGraphRuntime(graph);
    auto &audio = GET_MODULE(Audio);
    const auto existing = runtime.sounds.find(node);
    const auto epoch = runtime.sound_epochs.find(node);
    if (directive.phase == Phase::idle) {
        if (existing != runtime.sounds.end()) {
            audio.stopSound(existing->second);
            runtime.sounds.erase(existing);
        }
        if (epoch != runtime.sound_epochs.end()) runtime.sound_epochs.erase(epoch);
        if (const auto status = runtime.voice_status.find(node); status != runtime.voice_status.end())
            runtime.voice_status.erase(status);
        return true;
    }
    const bool restart = existing == runtime.sounds.end() || epoch == runtime.sound_epochs.end() ||
                         epoch->second != directive.epoch;
    std::optional<Audio::PreparedSound> prepared;
    if (restart) prepared = audio.prepareSound(source->second, audioVoiceBusFromName(directive.bus));
    const auto length = prepared ? Audio::lengthSeconds(*prepared) : audio.voiceLengthSeconds(existing->second);
    // The follow tolerance of decision 3: one frame, and 10 ms for the jitter
    // between the frame clock and the mixer.
    Audio::VoiceDrive drive;
    drive.tolerance = (std::max)(GET_MODULE(EngineTime).dt(), 0.0) + 0.010;
    Audio::VoiceStart start;
    start.bus = audioVoiceBusFromName(directive.bus);
    if (directive.follows_position) {
        drive.position = directive.position;
        drive.paused = directive.phase == Phase::paused || !directive.advancing;
        start.start = length > 0.0 ? std::fmod(directive.position, length) : 0.0;
        if (start.start < 0.0) start.start += length;
    } else {
        // The integrated form: value.playback's time map over the source.
        auto settings = directive.mapping;
        if (directive.end_from_source) settings.source_end = length;
        settings.duration = settings.source_end - settings.source_begin;
        if (!(settings.duration > 0.0))
            throw std::invalid_argument("audio playback range is empty: " + std::string{node});
        const auto time = Animation::mapCurveTime(settings, directive.elapsed);
        const bool waiting = directive.elapsed < settings.delay;
        drive.rate = static_cast<float>(std::abs(settings.speed));
        start.start = time.source_time;
        if (directive.native) {
            // The backend holds the loop point, so the voice runs on its own
            // clock and is never sought for the ordinary drift between the
            // mixer and the frames. A gap no drift explains -- a set_time, a
            // frame the engine clock clamped -- still brings it back to the
            // graph's time.
            drive.paused = directive.phase == Phase::paused || waiting;
            if (time.active) {
                drive.position = time.source_time;
                drive.tolerance = (std::max)(drive.tolerance, nativeResyncSeconds);
                drive.restart_ended = false;
            }
            start.loop = settings.wrap == Animation::CurveWrap::repeat;
            start.loop_begin = settings.source_begin;
            start.loop_end = settings.source_end;
            if (!start.loop) start.end = settings.source_end;
        } else {
            drive.position = time.source_time;
            drive.paused = directive.phase == Phase::paused || !time.active || !directive.advancing;
        }
    }
    start.rate = drive.rate;
    start.paused = drive.paused;
    // CT-1: start.start is the source second the graph asks for in both
    // forms (the position modulo the source, or the time map's source time).
    runtime.voice_status.insert_or_assign(std::string{node},
                                          GraphRuntime::VoiceStatus{drive.paused, start.start});
    if (restart) {
        const auto handle = audio.playVoice(*prepared, start);
        if (existing != runtime.sounds.end()) {
            audio.stopSound(existing->second);
            existing->second = handle;
        } else runtime.sounds.emplace(std::string{node}, handle);
        runtime.sound_epochs.insert_or_assign(std::string{node}, directive.epoch);
        return true;
    }
    audio.driveVoice(existing->second, drive);
    return true;
#else
    (void)graph;
    (void)node;
    (void)directive;
    return false;
#endif
}

bool SceneLoader::applyGraphAudioValue(const std::shared_ptr<const ValueGraph::Graph> &graph, std::string_view node,
                                       bool voice, ValueGraph::AudioValueKind kind, float value) {
    // A sink naming a target rather than a voice has nothing to resolve here.
    if (!voice) return false;
    const auto sound = graphSound(graph, node);
    // A connected producer can exist before start, or after reset.
    if (!sound) return true;
#if PELICAN_WITH_AUDIO
    auto &audio = GET_MODULE(Audio);
    return kind == ValueGraph::AudioValueKind::volume
        ? audio.rampSoundVolume(*sound, value, (std::max)(GET_MODULE(EngineTime).dt(), 0.0))
        : audio.setSoundPitch(*sound, value);
#else
    (void)kind;
    (void)value;
    return false;
#endif
}

SceneLoader::GraphAudioWriters SceneLoader::graphAudioWriters(
    const std::shared_ptr<const ValueGraph::Graph> &graph, PreparedGraphRuntimeTransfer *staged) {
    GraphAudioWriters writers;
    voice_graph = graph;
    writers.voices = [this, graph, staged](std::string_view node, const ValueGraph::AudioVoiceDirective &directive) {
        if (staged) return staged->stageAudioVoice(node, directive);
        return applyGraphVoice(graph, node, directive);
    };
    writers.sounds = [this, graph](std::string_view node) { return graphSound(graph, node); };
    writers.values = [this, graph, staged](std::string_view node, bool voice, ValueGraph::AudioValueKind kind,
                                           float value) {
        if (staged) return staged->stageAudioValue(node, voice, kind, value);
        return applyGraphAudioValue(graph, node, voice, kind, value);
    };
    return writers;
}

void SceneLoader::stopGraphVoices() noexcept {
#if PELICAN_WITH_AUDIO
    auto *audio = FastModuleContainer::tryGet<Audio>();
    for (auto &[graph, runtime] : graph_runtime) {
        (void)graph;
        if (audio)
            for (const auto &[node, sound] : runtime.sounds) {
                try {
                    audio->stopSound(sound);
                } catch (const std::exception &error) {
                    if (logger) LOG_ERROR(logger, "graph voice {} did not stop at the scene commit: {}", node, error.what());
                }
            }
        runtime.sounds.clear();
        runtime.sound_epochs.clear();
    }
#endif
}

std::optional<SoundHandle> SceneLoader::graphSound(const std::shared_ptr<const ValueGraph::Graph> &graph,
                                                std::string_view node) const {
    const auto runtime = graph_runtime.find(graph);
    if (runtime == graph_runtime.end()) return std::nullopt;
    const auto sound = runtime->second.sounds.find(node);
    return sound == runtime->second.sounds.end() ? std::nullopt : std::optional{sound->second};
}

std::vector<SceneLoader::GraphVoiceStatus> SceneLoader::graphVoiceStatus() const {
    std::vector<GraphVoiceStatus> result;
    const auto graph = voice_graph.lock();
    if (!graph) return result;
    // A scene commit makes the runtime again; until the next frame its voices
    // are none.
    const auto runtime = graph_runtime.find(graph);
    for (const auto &[node, asset] : graph->audioSources()) {
        (void)asset;
        GraphVoiceStatus entry{node, GraphVoiceStatus::State::idle, 0.0};
#if PELICAN_WITH_AUDIO
        const auto *audio = FastModuleContainer::tryGet<Audio>();
        if (runtime != graph_runtime.end() && audio != nullptr) {
            const auto sound = runtime->second.sounds.find(node);
            const auto status = runtime->second.voice_status.find(node);
            // A handle the mixer no longer knows (Audio::stopAll) is no voice.
            if (sound != runtime->second.sounds.end() && status != runtime->second.voice_status.end() &&
                audio->soundState(sound->second)) {
                entry.state = status->second.held ? GraphVoiceStatus::State::paused
                                                  : GraphVoiceStatus::State::playing;
                entry.position_seconds = status->second.position_seconds;
            }
        }
#endif
        result.push_back(std::move(entry));
    }
    return result;
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
    for (const auto &[node, epoch] : old.sound_epochs)
        if (prepared->retained_sounds_.contains(node)) prepared->retained_sound_epochs_.emplace(node, epoch);
    return prepared;
}

ValueGraph::ParameterValues SceneLoader::PreparedGraphRuntimeTransfer::parameters() const {
    return parameterValues(prepared_.begin()->second.parameters);
}

ValueGraph::EvaluationState &SceneLoader::PreparedGraphRuntimeTransfer::evaluation() {
    return prepared_.begin()->second.evaluation;
}

bool SceneLoader::PreparedGraphRuntimeTransfer::stageAudioVoice(
    std::string_view node, const ValueGraph::AudioVoiceDirective &directive) {
    audio_voices_.emplace_back(node, directive);
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
    for (const auto &[node, directive] : audio_voices_)
        if (!scene_.applyGraphVoice(graph, node, directive))
            throw std::runtime_error("published graph voice is unavailable: " + node);
    for (const auto &value : audio_values_)
        if (!scene_.applyGraphAudioValue(graph, value.node, true, value.kind, value.value))
            throw std::runtime_error("published graph voice is unavailable: " + value.node);
    audio_voices_.clear();
    audio_values_.clear();
}

void SceneLoader::PreparedGraphRuntimeTransfer::commit() noexcept {
    if (committed_) return;
    const auto old = scene_.graph_runtime.find(previous_);
    if (old != scene_.graph_runtime.end()) {
        for (const auto &[node, sound] : retained_sounds_) {
            old->second.sounds.erase(node);
            if (const auto epoch = old->second.sound_epochs.find(node); epoch != old->second.sound_epochs.end())
                old->second.sound_epochs.erase(epoch);
        }
        if (!prepared_.empty()) {
            prepared_.begin()->second.sounds.swap(retained_sounds_);
            prepared_.begin()->second.sound_epochs.swap(retained_sound_epochs_);
        }
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
