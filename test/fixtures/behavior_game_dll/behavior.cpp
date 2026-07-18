#include <behavior.hpp>

#include <string>

namespace {

struct ProjectBehaviorParams {
    std::string label;

    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&ProjectBehaviorParams::label>("label"),
                           "default"));
};

class ProjectBehavior final : public Pelican::Behavior {
  public:
    using Params = ProjectBehaviorParams;

    void onInit(Pelican::BehaviorContext &ctx) override {
        ctx.logInfo("WP155 behavior init label=" + ctx.params<Params>().label);
    }

    void onEvent(const Pelican::SceneLoaded &, Pelican::BehaviorContext &ctx) {
        ctx.logInfo("WP155 behavior event label=" + ctx.params<Params>().label);
    }

    void onUpdate(Pelican::BehaviorContext &ctx) override {
        ctx.logInfo("WP155 behavior update label=" + ctx.params<Params>().label);
    }

    void onDestroy(Pelican::BehaviorContext &ctx) noexcept override {
        ctx.logInfo("WP155 behavior destroy label=" + ctx.params<Params>().label);
    }
};

} // namespace

PELICAN_REGISTER_BEHAVIOR(ProjectBehavior, "wp155_project_behavior", 1);
