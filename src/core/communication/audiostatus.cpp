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
nlohmann::json voicesJson(const Audio *audio) {
    auto voices = nlohmann::json::array();
    const auto *scene = FastModuleContainer::tryGet<SceneLoader>();
    if (scene == nullptr) return voices;
    const auto *time = FastModuleContainer::tryGet<EngineTime>();
    const bool session = time == nullptr || time->playState() != EngineTime::PlayState::edit;
    const bool held = audio != nullptr && audio->allPaused();
    using State = SceneLoader::GraphVoiceStatus::State;
    for (const auto &voice : scene->graphVoiceStatus()) {
        auto state = session ? voice.state : State::idle;
        if (state == State::playing && held) state = State::paused;
        voices.push_back(nlohmann::json{
            {"node", voice.node},
            {"state", state == State::idle ? "idle" : state == State::paused ? "paused" : "playing"},
            {"position_seconds", state == State::idle ? 0.0 : voice.position_seconds}});
    }
    return voices;
}
} // namespace
#endif

nlohmann::json audioStatusJson() {
#if PELICAN_WITH_AUDIO
    const auto *audio = FastModuleContainer::tryGet<Audio>();
    if (audio == nullptr)
        return nlohmann::json{{"available", true}, {"backend", nullptr}, {"voices", voicesJson(nullptr)}};
    nlohmann::json status{{"available", true},
                          {"backend", std::string{audioBackendKindName(audio->backendKind())}}};
    if (!audio->backendFallbackReason().empty()) status["fallback_reason"] = audio->backendFallbackReason();
    status["voices"] = voicesJson(audio);
    return status;
#else
    return nlohmann::json{{"available", false}};
#endif
}

} // namespace Pelican::internal
