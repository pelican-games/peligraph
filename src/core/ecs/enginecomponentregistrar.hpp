#pragma once

#include "../userpublic/details/component/registerer.hpp"

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Pelican::internal {

// This type and every function in this header are host-only.  The public
// component registrar never accepts an owner value from game-DLL code.
class EngineComponentRegisterer final {
  public:
    template <class Component>
    RegistrationToken registerComponent(std::string name) {
        auto &registerer = getComponentRegisterer();
        return registerer.__registerEngineComponent(
            ComponentIdByType<Component>::value, sizeof(Component),
            registerer.componentLoader<Component>(std::move(name)));
    }
};

EngineComponentRegisterer &getEngineComponentRegisterer();

std::span<const std::string_view>
grandfatheredEngineComponentNames() noexcept;

void unregisterComponent(RegistrationToken token);
void unregisterComponents(RegistrationOwner owner) noexcept;
std::size_t componentRegistrationCount(RegistrationOwner owner) noexcept;
std::vector<std::string>
componentRegistrationNames(RegistrationOwner owner);

} // namespace Pelican::internal
