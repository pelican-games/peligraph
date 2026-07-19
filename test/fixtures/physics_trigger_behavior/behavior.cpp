#include <behavior.hpp>

namespace {

struct TriggerBehaviorParams {
    static constexpr auto schema =
        Pelican::structFields(Pelican::behaviorParamsPolicy);
};

class TriggerBehavior final : public Pelican::Behavior {
  public:
    using Params = TriggerBehaviorParams;

    void onEvent(const Pelican::OverlapEnter &event,
                 Pelican::BehaviorContext &ctx) {
        if (event.self != ctx.self()) return;
        ctx.logInfo("WP179 typed OverlapEnter self=" +
                    Pelican::toString(event.self) + " other=" +
                    Pelican::toString(event.other));
    }
};

} // namespace

PELICAN_REGISTER_BEHAVIOR(TriggerBehavior, "wp179_trigger_behavior", 1);
