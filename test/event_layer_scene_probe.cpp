#include "../src/core/userpublic/gamesystem.hpp"

#include <cstdint>

namespace {

struct Wp56InjectedEvent {
    std::uint64_t seed = 0;

    static constexpr auto pelican_payload =
        Pelican::payloadFields(Pelican::field<&Wp56InjectedEvent::seed>("seed"));

    template <class T> void ref(T &ar) {
        ar.prop("seed", seed);
    }
};

struct Wp56EventLayerProbeSystem {
    void onEvent(const Wp56InjectedEvent &event, Pelican::GameContext &ctx) {
        ctx.setSeed(event.seed);
    }

    void onEvent(const Pelican::SceneLoaded &event, Pelican::GameContext &ctx) {
        if (event.scene_name == "scene_flow_second") {
            ctx.setSeed(5602);
        } else if (event.scene_name == "default_scene") {
            ctx.setSeed(5601);
        }
    }
};

} // namespace

PELICAN_REGISTER_EVENT(Wp56InjectedEvent);
PELICAN_REGISTER_SYSTEM(Wp56EventLayerProbeSystem, 5);
