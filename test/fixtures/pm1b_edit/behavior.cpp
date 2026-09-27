#include "api.hpp"

#include <behavior.hpp>
#include <gamecontext.hpp>
#include <gameobjects.hpp>
#include <gamesystem.hpp>

#include <string>

// PM-1b: a game DLL whose behaviors report every piece of their lifecycle, so
// the host can tell that the edit state runs none of it.

namespace {

Pelican::Pm1bFixture::Counts counts;

struct MoverParams {
    std::int32_t step = 1;
    bool spawn = false;
    // PM-2: a sound to play and a scene to load on the first update.
    std::string sound;
    std::string load_scene;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&MoverParams::step>("step"), 1),
        Pelican::defaulted(Pelican::field<&MoverParams::spawn>("spawn"), false),
        Pelican::defaulted(Pelican::field<&MoverParams::sound>("sound"), ""),
        Pelican::defaulted(Pelican::field<&MoverParams::load_scene>("load_scene"), ""));
};

// Moves its object along +x by step / 2 on every update. With `spawn`, its
// first update spawns one pm1b_spark next to it; with `sound` it plays that
// sound, and with `load_scene` it asks for that scene.
class MoverBehavior final : public Pelican::Behavior {
    bool spawned_ = false;
    bool first_done_ = false;

  public:
    using Params = MoverParams;

    MoverBehavior() { ++counts.constructed; }

    void onInit(Pelican::BehaviorContext &) override { ++counts.inits; }

    void onUpdate(Pelican::BehaviorContext &context) override {
        ++counts.updates;
        const auto &params = context.params<Params>();
        auto transform = Pelican::GameObjects::localTransform(context.self());
        transform.pos.x += static_cast<float>(params.step) * 0.5f;
        (void)Pelican::GameObjects::setLocalTransform(context.self(), transform);
        if (params.spawn && !spawned_) {
            spawned_ = true;
            ++counts.spawns;
            try {
                Pelican::SpawnParams spawn;
                spawn.transform.pos = Pelican::vec3{transform.pos.x, 1.0f, 0.0f};
                (void)Pelican::GameContext{}.spawn("pm1b_spark", spawn);
            } catch (...) {
                ++counts.spawn_errors;
            }
        }
        if (!first_done_) {
            first_done_ = true;
            if (!params.sound.empty()) {
                ++counts.sounds;
                try {
                    (void)Pelican::GameContext{}.audio().playSound(params.sound);
                } catch (...) {
                    ++counts.sound_errors;
                }
            }
            if (!params.load_scene.empty()) {
                ++counts.scene_loads;
                try {
                    Pelican::GameContext{}.loadScene(params.load_scene);
                } catch (...) {
                    ++counts.scene_load_errors;
                }
            }
        }
    }

    void onDestroy(Pelican::BehaviorContext &) noexcept override { ++counts.destroys; }
};

struct SparkParams {
    std::string label;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&SparkParams::label>("label"), ""));
};

class SparkBehavior final : public Pelican::Behavior {
  public:
    using Params = SparkParams;

    void onInit(Pelican::BehaviorContext &) override { ++counts.spark_inits; }
    void onDestroy(Pelican::BehaviorContext &) noexcept override { ++counts.spark_destroys; }
};

// PM-2: a game system whose count lives in its own instance, which the DLL
// keeps until it is loaded again.
struct TickerSystem {
    std::uint64_t updates = 0;

    void update(Pelican::GameContext &) { counts.system_updates = ++updates; }
};

} // namespace

PELICAN_REGISTER_SYSTEM(TickerSystem, 60);
PELICAN_REGISTER_BEHAVIOR(MoverBehavior, "pm1b_mover", 1);
PELICAN_REGISTER_BEHAVIOR(SparkBehavior, "pm1b_spark", 1);

PELICAN_PM1B_FIXTURE_API void pelican_pm1b_counts(Pelican::Pm1bFixture::Counts *out) {
    *out = counts;
}

PELICAN_PM1B_FIXTURE_API void pelican_pm1b_reset() { counts = {}; }
