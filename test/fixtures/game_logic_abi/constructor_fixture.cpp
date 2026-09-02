#include "constructor_probe.hpp"

#include <cstdint>

#if defined(WP381_FROZEN_V1_BEHAVIOR)

#include <behavior.hpp>

namespace {

struct FrozenParams {
    static constexpr auto schema =
        Pelican::structFields(Pelican::behaviorParamsPolicy);
};

class FrozenBehavior final : public Pelican::Behavior {
  public:
    using Params = FrozenParams;
};

struct ConstructorProbe {
    ConstructorProbe() {
        wp381_fixture::writeProbe(
            reinterpret_cast<const void *>(&wp381_fixture::writeProbe), 0);
    }
};

ConstructorProbe constructor_probe;

} // namespace

PELICAN_REGISTER_BEHAVIOR(FrozenBehavior, "wp381_frozen_v1", 1);

#elif defined(WP381_CURRENT_V2_CONTROL)

#include <gamelogic.hpp>

namespace {

struct ConstructorProbe {
    ConstructorProbe() {
        const auto nonce = Pelican::pelican_game_logic_epoch_v2();
        wp381_fixture::writeProbe(
            reinterpret_cast<const void *>(&wp381_fixture::writeProbe),
            nonce);
    }
};

ConstructorProbe constructor_probe;

} // namespace

#else
#error A WP381 constructor fixture role must be selected
#endif
