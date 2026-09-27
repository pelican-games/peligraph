// PM-3a: a behavior that moves its object a fixed distance per game step, so
// a Studio scenario can tell "moving", "stopped" and "one step" apart from the
// object's runtime transform alone. The distance is a power of two, so the
// position after n steps is exactly n times it.
#include <behavior.hpp>

#include <string>

namespace {

struct MoverParams {
    double step = 0.25;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&MoverParams::step>("step"), 0.25));
};

class MoverBehavior final : public Pelican::Behavior {
  public:
    using Params = MoverParams;

    void onUpdate(Pelican::BehaviorContext &ctx) override {
        // Counted, not integrated from dt: a paused frame runs no behavior,
        // and a step runs it exactly once.
        ++steps_;
        auto transform = ctx.localTransform(ctx.self());
        transform.pos.x = static_cast<float>(ctx.params<Params>().step * static_cast<double>(steps_));
        (void)ctx.setLocalTransform(ctx.self(), transform);
    }

  private:
    unsigned long long steps_ = 0;
};

} // namespace

PELICAN_REGISTER_BEHAVIOR(MoverBehavior, "pm3a_mover", 1);
