#include "audiostatus.hpp"

#if PELICAN_WITH_AUDIO
#include "../appflow/enginetime.hpp"
#include "../audio/audio.hpp"
#include "../container.hpp"
#include "../loader/scene.hpp"

#include <string>
#endif

namespace Pelican::internal {

#if PELICAN_WITH_AUDIO
namespace {
// CT-1 voices, and (AT-1 decision 6) the master among them.
struct GraphVoicesJson {
    nlohmann::json voices = nlohmann::json::array();
    nlohmann::json master = nullptr;
};

GraphVoicesJson voicesJson(const Audio *audio) {
    GraphVoicesJson result;
    const auto *scene = FastModuleContainer::tryGet<SceneLoader>();
    if (scene == nullptr) return result;
    const auto *time = FastModuleContainer::tryGet<EngineTime>();
    const bool session = time == nullptr || time->playState() != EngineTime::PlayState::edit;
    const bool held = audio != nullptr && audio->allPaused();
    using State = SceneLoader::GraphVoiceStatus::State;
    for (const auto &voice : scene->graphVoiceStatus()) {
        auto state = session ? voice.state : State::idle;
        if (state == State::playing && held) state = State::paused;
        const auto position = state == State::idle ? 0.0 : voice.position_seconds;
        result.voices.push_back(nlohmann::json{
            {"node", voice.node},
            {"state", state == State::idle ? "idle" : state == State::paused ? "paused" : "playing"},
            {"position_seconds", position}});
        // A master with a voice. Where the mixer has no cursor (null) it is
        // heard where the graph put it.
        if (voice.master && state != State::idle && result.master.is_null()) {
            const auto heard = voice.heard_position_seconds.value_or(position);
            result.master = nlohmann::json{{"node", voice.node},
                                           {"position_seconds", position},
                                           {"heard_position_seconds", heard},
                                           {"offset_seconds", voice.offset_seconds.value_or(position - heard)}};
        }
    }
    return result;
}

std::string_view clockSourceName(Audio::ClockSource source) {
    switch (source) {
    case Audio::ClockSource::device: return "device";
    case Audio::ClockSource::offline: return "offline";
    case Audio::ClockSource::game_clock: return "game_clock";
    }
    return "game_clock";
}

// AT-1 decision 6: the audio clock the frame read. Without an Audio module
// the game clock stands in for it, as on the null backend.
nlohmann::json clockJson(const Audio *audio) {
    const auto *time = FastModuleContainer::tryGet<EngineTime>();
    const auto game_time = time != nullptr ? time->now() : 0.0;
    Audio::ClockReading reading;
    if (audio != nullptr) reading = audio->clock(game_time);
    else reading.audio_time = game_time;
    return nlohmann::json{{"source", std::string{clockSourceName(reading.source)}},
                          {"sample_rate", reading.sample_rate},
                          {"audio_time", reading.audio_time},
                          {"played_frames", reading.played_frames},
                          {"output_latency_seconds", reading.output_latency_seconds},
                          {"latency_source", reading.latency_estimated ? "estimated" : "none"},
                          {"drift_seconds", reading.drift_seconds}};
}
} // namespace
#endif

nlohmann::json audioStatusJson() {
#if PELICAN_WITH_AUDIO
    const auto *audio = FastModuleContainer::tryGet<Audio>();
    auto graph = voicesJson(audio);
    if (audio == nullptr)
        return nlohmann::json{{"available", true}, {"backend", nullptr}, {"voices", std::move(graph.voices)},
                              {"clock", clockJson(nullptr)}, {"master", std::move(graph.master)}};
    nlohmann::json status{{"available", true},
                          {"backend", std::string{audioBackendKindName(audio->backendKind())}}};
    if (!audio->backendFallbackReason().empty()) status["fallback_reason"] = audio->backendFallbackReason();
    status["voices"] = std::move(graph.voices);
    status["clock"] = clockJson(audio);
    status["master"] = std::move(graph.master);
    return status;
#else
    return nlohmann::json{{"available", false}};
#endif
}

} // namespace Pelican::internal
