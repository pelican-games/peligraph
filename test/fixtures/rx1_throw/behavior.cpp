#include <behavior.hpp>
#include <stdexcept>
struct ThrowParams {
    bool on_init = false;
    std::string message = "fixture behavior failure";
    static constexpr auto schema = Pelican::structFields(
        Pelican::behaviorParamsPolicy,
        Pelican::defaulted(Pelican::field<&ThrowParams::message>("message"), "fixture behavior failure"),
        Pelican::defaulted(Pelican::field<&ThrowParams::on_init>("on_init"), false));
};
class ThrowBehavior : public Pelican::Behavior {
  public:
    using Params = ThrowParams;
    void onInit(Pelican::BehaviorContext &ctx) override {
        if (ctx.params<Params>().on_init) throw std::runtime_error(ctx.params<Params>().message);
    }
    void onUpdate(Pelican::BehaviorContext &ctx) override {
        throw std::runtime_error(ctx.params<Params>().message);
    }
};
PELICAN_REGISTER_BEHAVIOR(ThrowBehavior, "rx1_throw", 1);
