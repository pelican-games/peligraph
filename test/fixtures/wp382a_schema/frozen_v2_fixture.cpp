#include <details/component/registerer.hpp>
#include <details/ecs/componentdeclare.hpp>

#include <cstdint>

namespace {

struct FrozenV2Component {
    std::uint32_t value = 0;

    template <class Archive> void ref(Archive &archive) {
        archive.prop("value", value);
    }
};

} // namespace

DECLARE_COMPONENT(FrozenV2Component, 60);

namespace {

class FrozenV2ComponentRegistration final {
  public:
    FrozenV2ComponentRegistration() {
        (void)Pelican::internal::getComponentRegisterer()
            .registerComponent<FrozenV2Component>(
                "wp382a_frozen_v2_component");
    }
};

FrozenV2ComponentRegistration component_registration;

} // namespace
