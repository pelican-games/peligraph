// PM-3c: a behavior that spawns one object on its first game step, so a
// Studio scenario can see a spawned object appear in the Outliner while the
// game plays and go when it stops. The spawn is named, and stands still at a
// place the scenario reads back in the Inspector.
#include <behavior.hpp>
#include <gamecontext.hpp>

#include <string>

namespace {

struct NoParams {
    static constexpr auto schema = Pelican::structFields(Pelican::behaviorParamsPolicy);
};

class SpawnerBehavior final : public Pelican::Behavior {
  public:
    using Params = NoParams;

    void onUpdate(Pelican::BehaviorContext &ctx) override {
        // Once per run: a Play after a Stop builds this behavior again.
        if (spawned_) return;
        spawned_ = true;
        Pelican::SpawnParams spawn;
        spawn.name = "Shot";
        spawn.transform.pos = Pelican::vec3{1.5f, 2.0f, -0.5f};
        try {
            (void)Pelican::GameContext{}.spawn("pm3c_shot", spawn);
        } catch (const Pelican::SpawnError &error) {
            ctx.logInfo(std::string{"PM-3c spawn refused: "} + std::string{error.code()});
        }
    }

  private:
    bool spawned_ = false;
};

// What the prefab carries: nothing moves it.
class ShotBehavior final : public Pelican::Behavior {
  public:
    using Params = NoParams;
};

} // namespace

PELICAN_REGISTER_BEHAVIOR(SpawnerBehavior, "pm3c_spawner", 1);
PELICAN_REGISTER_BEHAVIOR(ShotBehavior, "pm3c_shot", 1);
